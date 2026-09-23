#include "input.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears
{
namespace
{

std::mutex g_mutex;
PadState g_pad;
uint32_t g_packet = 0;
bool g_haveWindow = false;
bool g_remoteActive = false;
HostPadSampler g_hostSampler = nullptr;
void *g_hostContext = nullptr;

// One entry of GEARS_INPUT_SCRIPT: hold `buttons` from `atMs` until the next
// entry's time.
struct ScriptStep
{
    // The step's time. `byFrame` decides what `at` MEANS: guest frames when the
    // token was written "f1500:", milliseconds otherwise. Both are monotonic
    // and the cursor logic below is identical for the two, so the only place
    // the distinction exists is where "now" is read.
    uint64_t atMs = 0;
    bool byFrame = false;
    uint16_t buttons = 0;
    // Analogue stick deflections. Buttons alone cannot drive this title: Gears
    // moves and aims on the STICKS, so a script limited to buttons can walk the
    // menus but can never test whether the game responds to a player at all.
    int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
    // Trigger pulls. Gears chooses between some paths and aims with them.
    uint8_t leftTrigger = 0, rightTrigger = 0;
};
std::vector<ScriptStep> g_script;
uint64_t (*g_frameSource)() = nullptr;
size_t g_scriptCursor = 0;
// Set once every scripted step has fired. From then on the script no longer
// owns the pad, so a remote controller may continue the run from where the
// script left it.
std::atomic<bool> g_scriptFinished{false};
std::chrono::steady_clock::time_point g_start;

void PublishLocked(const PadState &next)
{
    if (next == g_pad)
        return;
    g_pad = next;
    // The console's contract: the packet number changes only when the state
    // does, so a title that compares packet numbers sees real edges.
    ++g_packet;
    lucent::debug("input", "pad state {}: buttons {:#06x} triggers {},{} stick L({},{}) R({},{})",
                  g_packet, next.buttons, next.leftTrigger, next.rightTrigger, next.thumbLX,
                  next.thumbLY, next.thumbRX, next.thumbRY);
}

void Publish(const PadState &next)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    PublishLocked(next);
}

void PublishHost(const PadState &next)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    // Recheck under the same lock SetRemotePad uses. A check before sampling
    // leaves a race where the final host sample overwrites a remote command.
    if (!g_remoteActive)
        PublishLocked(next);
}

// A stick deflection named in a script step, e.g. "LY+" or "RX-". Returns false
// if the name is not a stick, so the caller can try it as a button.
bool StickByName(std::string_view name, ScriptStep &into)
{
    if (name.size() < 3)
        return false;
    const bool negative = name.back() == '-';
    if (!negative && name.back() != '+')
        return false;
    const std::string_view axis = name.substr(0, name.size() - 1);
    // Full deflection: a script is for reproducible tests, so it presses all the
    // way rather than guessing at a partial value.
    const int16_t v = negative ? int16_t(-32767) : int16_t(32767);
    if (axis == "LX")
    {
        into.thumbLX = v;
        return true;
    }
    if (axis == "LY")
    {
        into.thumbLY = v;
        return true;
    }
    if (axis == "RX")
    {
        into.thumbRX = v;
        return true;
    }
    if (axis == "RY")
    {
        into.thumbRY = v;
        return true;
    }
    return false;
}

// A trigger named in a script step, "LT" or "RT", pulled all the way. Returns
// false if the name is not a trigger.
bool TriggerByName(std::string_view name, ScriptStep &into)
{
    if (name == "LT")
    {
        into.leftTrigger = UINT8_MAX;
        return true;
    }
    if (name == "RT")
    {
        into.rightTrigger = UINT8_MAX;
        return true;
    }
    return false;
}

// "3000:START,3200:,5000:A" -- at 3000 ms hold START, at 3200 ms release
// everything, at 5000 ms hold A. Times are milliseconds since start-up.
//
// A step may also name STICK deflections: LX/LY/RX/RY suffixed with '+' or '-',
// e.g. "9000:LY+" walks forward. Y follows the console's convention, positive up.
// LT and RT pull the triggers.
//
// Names within a step are combined with '&', not '+': '+' is a stick SIGN, and
// using it for both roles is ambiguous ("LY++A" could split either way). '&'
// separates, so "9000:LY+&A" walks forward while holding A.
void ParseScript(std::string_view text)
{
    while (!text.empty())
    {
        const size_t comma = text.find(',');
        std::string_view step = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        const size_t colon = step.find(':');
        if (colon == std::string_view::npos)
        {
            lucent::warn("input", "GEARS_INPUT_SCRIPT step \"{}\" has no time", step);
            continue;
        }
        ScriptStep entry;
        std::string_view timeText = step.substr(0, colon);
        // "f1500:A" is guest FRAME 1500; "1500:A" is 1500 ms. A script that
        // mixes the two is accepted -- each step carries its own unit -- but
        // the report below says how many of each there are, because a mixed
        // script is nearly always a typo.
        if (!timeText.empty() && (timeText.front() == 'f' || timeText.front() == 'F'))
        {
            entry.byFrame = true;
            timeText.remove_prefix(1);
        }
        if (std::from_chars(timeText.data(), timeText.data() + timeText.size(), entry.atMs).ec !=
            std::errc{})
        {
            lucent::warn("input", "GEARS_INPUT_SCRIPT time \"{}\" is not a number", timeText);
            continue;
        }
        std::string_view buttons = step.substr(colon + 1);
        while (!buttons.empty())
        {
            const size_t sep = buttons.find('&');
            const std::string_view name = buttons.substr(0, sep);
            if (!StickByName(name, entry) && !TriggerByName(name, entry))
            {
                uint16_t button = 0;
                if (PadButtonByName(name, button))
                    entry.buttons |= button;
                else
                    lucent::warn("input",
                                 "unknown button name \"{}\" in"
                                 " GEARS_INPUT_SCRIPT",
                                 name);
            }
            if (sep == std::string_view::npos)
                break;
            buttons = buttons.substr(sep + 1);
        }
        g_script.push_back(entry);
    }
    std::stable_sort(g_script.begin(), g_script.end(),
                     [](const ScriptStep &a, const ScriptStep &b) { return a.atMs < b.atMs; });
}

// Whether a script still drives the pad: it has steps left to fire.
bool ScriptOwnsPad()
{
    return !g_script.empty() && !g_scriptFinished.load(std::memory_order_acquire);
}

} // namespace

void InitialiseInput(bool haveWindow)
{
    g_haveWindow = haveWindow;
    g_start = std::chrono::steady_clock::now();

    const std::string &script = lucent::config::text("INPUT_SCRIPT");
    if (!script.empty())
    {
        ParseScript(script);
        lucent::info("input", "scripted input: {} steps from GEARS_INPUT_SCRIPT", g_script.size());
    }

    if (!g_haveWindow && g_script.empty())
    {
        lucent::info("input", "no input source (headless, no GEARS_INPUT_SCRIPT);"
                              " the pad reports disconnected");
    }
}

bool PadConnected()
{
    return ReadPadSnapshot().connected;
}

PadSnapshot ReadPadSnapshot()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return {g_remoteActive || g_haveWindow || !g_script.empty(), g_packet, g_pad};
}

PadState CurrentPad(uint32_t &packetNumber)
{
    const PadSnapshot snapshot = ReadPadSnapshot();
    packetNumber = snapshot.packet;
    return snapshot.state;
}

bool PadButtonByName(std::string_view name, uint16_t &button)
{
    struct NamedButton
    {
        std::string_view name;
        uint16_t value;
    };
    static constexpr NamedButton buttons[] = {
        {"UP", kPadDpadUp},
        {"DOWN", kPadDpadDown},
        {"LEFT", kPadDpadLeft},
        {"RIGHT", kPadDpadRight},
        {"START", kPadStart},
        {"BACK", kPadBack},
        {"LTHUMB", kPadLeftThumb},
        {"RTHUMB", kPadRightThumb},
        {"LB", kPadLeftShoulder},
        {"RB", kPadRightShoulder},
        {"A", kPadA},
        {"B", kPadB},
        {"X", kPadX},
        {"Y", kPadY},
    };
    for (const NamedButton &candidate : buttons)
    {
        if (candidate.name == name)
        {
            button = candidate.value;
            return true;
        }
    }
    return false;
}

bool SetRemotePad(const PadState &state)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (ScriptOwnsPad())
        return false;
    g_remoteActive = true;
    PublishLocked(state);
    return true;
}

void ReleaseRemotePad()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_remoteActive)
        PublishLocked({});
}

void DisconnectRemotePad()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_remoteActive)
        return;
    g_remoteActive = false;
    PublishLocked({});
}

InputSource CurrentInputSource()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (ScriptOwnsPad())
        return InputSource::kScript;
    if (g_remoteActive)
        return InputSource::kRemote;
    if (g_haveWindow)
        return InputSource::kHost;
    return InputSource::kNone;
}

const char *InputSourceName(InputSource source)
{
    switch (source)
    {
    case InputSource::kNone:
        return "none";
    case InputSource::kHost:
        return "host";
    case InputSource::kScript:
        return "script";
    case InputSource::kRemote:
        return "remote";
    }
    return "unknown";
}

void SetGuestFrameSource(uint64_t (*source)())
{
    g_frameSource = source;
}

uint64_t CurrentGuestFrame()
{
    return g_frameSource ? g_frameSource() : 0;
}

void UpdateScriptedInput()
{
    // Called both from the presenter thread and from the guest's own
    // XamInputGetState, so the cursor needs its own lock (Publish takes the
    // state lock separately, after this one is released).
    static std::mutex scriptMutex;
    std::unique_lock<std::mutex> guard(scriptMutex);
    if (g_script.empty() || g_scriptCursor >= g_script.size())
        return;
    const uint64_t elapsed = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - g_start)
                                          .count());
    uint64_t frames = 0;
    if (g_frameSource)
    {
        frames = g_frameSource();
    }
    else
    {
        // Said once, and only if the script actually has a frame-indexed step:
        // a millisecond-only script needs no source and must not warn.
        static bool warned = false;
        if (!warned)
            for (const ScriptStep &st : g_script)
                if (st.byFrame)
                {
                    warned = true;
                    lucent::warn("input", "GEARS_INPUT_SCRIPT has frame-indexed"
                                          " step(s) but no guest frame source is registered, so"
                                          " NONE of them will ever fire. This run is not being"
                                          " driven the way the script says it is");
                    break;
                }
    }
    // Each step is due against its OWN unit.
    const auto due = [&](const ScriptStep &s2)
    { return s2.byFrame ? (frames >= s2.atMs) : (elapsed >= s2.atMs); };

    // EVERY DUE STEP IS CONSUMED AND ONLY THE LAST IS APPLIED, so a step whose
    // successor is already due is SKIPPED -- and it used to be skipped silently.
    // That matters because this script is the input side of every headless
    // measurement: the script only advances when the guest POLLS, so a press
    // between two polls never reaches the title at all. Observed in practice
    // with "1000:START,1500:LY+&A" -- the guest first polled at 1939 ms and the
    // START was simply gone, which is indistinguishable from a title that
    // ignored it unless the skip is reported.
    ScriptStep current;
    bool fired = false;
    uint64_t skipped = 0;
    uint64_t firstSkippedAt = 0;
    while (g_scriptCursor < g_script.size() && due(g_script[g_scriptCursor]))
    {
        if (fired)
        {
            if (skipped == 0)
                firstSkippedAt = current.atMs;
            ++skipped;
        }
        current = g_script[g_scriptCursor];
        ++g_scriptCursor;
        fired = true;
    }
    if (!fired)
        return;
    const bool finished = g_scriptCursor == g_script.size();
    guard.unlock();

    if (skipped != 0)
        lucent::warn("input",
                     "{} scripted step(s) were SKIPPED: the guest did not"
                     " poll between {} ms and {} ms, so only the {} ms step is applied."
                     " Space the steps further apart -- a press that is never polled never"
                     " reaches the title",
                     skipped, firstSkippedAt, elapsed, current.atMs);

    PadState next;
    next.buttons = current.buttons;
    next.thumbLX = current.thumbLX;
    next.thumbLY = current.thumbLY;
    next.thumbRX = current.thumbRX;
    next.thumbRY = current.thumbRY;
    next.leftTrigger = current.leftTrigger;
    next.rightTrigger = current.rightTrigger;
    Publish(next);
    lucent::info("input",
                 "scripted pad at {} ms: buttons {:#06x} triggers {},{} stick L({},{})"
                 " R({},{})",
                 elapsed, current.buttons, current.leftTrigger, current.rightTrigger,
                 current.thumbLX, current.thumbLY, current.thumbRX, current.thumbRY);
    // Only after its last state is published, so a remote write cannot be
    // overwritten by the script it followed.
    if (finished)
    {
        g_scriptFinished.store(true, std::memory_order_release);
        lucent::info("input",
                     "the input script has finished; a remote controller may take the pad");
    }
}

void SetHostPadSource(HostPadSampler sampler, void *context)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    g_hostSampler = sampler;
    g_hostContext = context;
}

void PollHostInput()
{
    UpdateScriptedInput();
    HostPadSampler sampler = nullptr;
    void *context = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_mutex);
        // A scripted run drives the pad itself; mixing the two would make the
        // script non-reproducible. A remote pad owns it until disconnected.
        if (!g_haveWindow || ScriptOwnsPad() || g_remoteActive)
            return;
        sampler = g_hostSampler;
        context = g_hostContext;
    }
    if (sampler != nullptr)
        PublishHost(sampler(context));
}

} // namespace gears
