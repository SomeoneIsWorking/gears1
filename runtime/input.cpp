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

#ifdef GEARS_HAVE_PRESENTER
#include <SDL3/SDL.h>
#endif

namespace gears
{
namespace
{

std::mutex g_mutex;
PadState g_pad;
uint32_t g_packet = 0;
bool g_haveWindow = false;

// One entry of GEARS_INPUT_SCRIPT: hold `buttons` from `atMs` until the next
// entry's time.
struct ScriptStep
{
    uint64_t atMs = 0;
    uint16_t buttons = 0;
};
std::vector<ScriptStep> g_script;
size_t g_scriptCursor = 0;
std::chrono::steady_clock::time_point g_start;

void Publish(const PadState& next)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (next == g_pad)
        return;
    g_pad = next;
    // The console's contract: the packet number changes only when the state
    // does, so a title that compares packet numbers sees real edges.
    ++g_packet;
}

uint16_t ButtonByName(std::string_view name)
{
    if (name == "UP") return kPadDpadUp;
    if (name == "DOWN") return kPadDpadDown;
    if (name == "LEFT") return kPadDpadLeft;
    if (name == "RIGHT") return kPadDpadRight;
    if (name == "START") return kPadStart;
    if (name == "BACK") return kPadBack;
    if (name == "LTHUMB") return kPadLeftThumb;
    if (name == "RTHUMB") return kPadRightThumb;
    if (name == "LB") return kPadLeftShoulder;
    if (name == "RB") return kPadRightShoulder;
    if (name == "A") return kPadA;
    if (name == "B") return kPadB;
    if (name == "X") return kPadX;
    if (name == "Y") return kPadY;
    lucent::warn("input", "unknown button name \"{}\" in GEARS_INPUT_SCRIPT", name);
    return 0;
}

// "3000:START,3200:,5000:A" -- at 3000 ms hold START, at 3200 ms release
// everything, at 5000 ms hold A. Times are milliseconds since start-up.
void ParseScript(std::string_view text)
{
    while (!text.empty())
    {
        const size_t comma = text.find(',');
        std::string_view step = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{}
                                               : text.substr(comma + 1);
        const size_t colon = step.find(':');
        if (colon == std::string_view::npos)
        {
            lucent::warn("input", "GEARS_INPUT_SCRIPT step \"{}\" has no time", step);
            continue;
        }
        ScriptStep entry;
        const std::string_view timeText = step.substr(0, colon);
        if (std::from_chars(timeText.data(), timeText.data() + timeText.size(),
                entry.atMs).ec != std::errc{})
        {
            lucent::warn("input", "GEARS_INPUT_SCRIPT time \"{}\" is not a number",
                timeText);
            continue;
        }
        std::string_view buttons = step.substr(colon + 1);
        while (!buttons.empty())
        {
            const size_t plus = buttons.find('+');
            entry.buttons |= ButtonByName(buttons.substr(0, plus));
            if (plus == std::string_view::npos)
                break;
            buttons = buttons.substr(plus + 1);
        }
        g_script.push_back(entry);
    }
    std::stable_sort(g_script.begin(), g_script.end(),
        [](const ScriptStep& a, const ScriptStep& b) { return a.atMs < b.atMs; });
}

#ifdef GEARS_HAVE_PRESENTER
SDL_Gamepad* g_gamepad = nullptr;

// The keyboard fallback, so the title is playable without a pad attached. The
// layout is the conventional one for Xbox-style controls on a keyboard.
struct KeyBinding
{
    SDL_Scancode key;
    uint16_t button;
};
constexpr KeyBinding kKeyBindings[] = {
    {SDL_SCANCODE_RETURN, kPadStart},
    {SDL_SCANCODE_ESCAPE, kPadBack},
    {SDL_SCANCODE_SPACE, kPadA},
    {SDL_SCANCODE_LSHIFT, kPadB},
    {SDL_SCANCODE_E, kPadX},
    {SDL_SCANCODE_Q, kPadY},
    {SDL_SCANCODE_UP, kPadDpadUp},
    {SDL_SCANCODE_DOWN, kPadDpadDown},
    {SDL_SCANCODE_LEFT, kPadDpadLeft},
    {SDL_SCANCODE_RIGHT, kPadDpadRight},
    {SDL_SCANCODE_1, kPadLeftShoulder},
    {SDL_SCANCODE_3, kPadRightShoulder},
};

struct PadAxisBinding
{
    SDL_GamepadButton button;
    uint16_t bit;
};
constexpr PadAxisBinding kPadBindings[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, kPadA},
    {SDL_GAMEPAD_BUTTON_EAST, kPadB},
    {SDL_GAMEPAD_BUTTON_WEST, kPadX},
    {SDL_GAMEPAD_BUTTON_NORTH, kPadY},
    {SDL_GAMEPAD_BUTTON_START, kPadStart},
    {SDL_GAMEPAD_BUTTON_BACK, kPadBack},
    {SDL_GAMEPAD_BUTTON_GUIDE, kPadGuide},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, kPadLeftThumb},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kPadRightThumb},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kPadLeftShoulder},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kPadRightShoulder},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, kPadDpadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDpadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadDpadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadDpadRight},
};

// The keyboard has no analogue stick, so W/A/S/D drives the left one and the
// arrow keys already serve the d-pad. Full deflection, because a keyboard has
// nothing in between.
constexpr int16_t kFullDeflection = 32767;
#endif

} // namespace

void InitialiseInput(bool haveWindow)
{
    g_haveWindow = haveWindow;
    g_start = std::chrono::steady_clock::now();

    const std::string& script = lucent::config::text("INPUT_SCRIPT");
    if (!script.empty())
    {
        ParseScript(script);
        lucent::info("input", "scripted input: {} steps from GEARS_INPUT_SCRIPT",
            g_script.size());
    }

    if (!g_haveWindow && g_script.empty())
    {
        lucent::info("input", "no input source (headless, no GEARS_INPUT_SCRIPT);"
            " the pad reports disconnected");
    }
}

bool PadConnected()
{
    return g_haveWindow || !g_script.empty();
}

PadState CurrentPad(uint32_t& packetNumber)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    packetNumber = g_packet;
    return g_pad;
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
        std::chrono::steady_clock::now() - g_start).count());

    uint16_t buttons = 0;
    bool fired = false;
    while (g_scriptCursor < g_script.size() && g_script[g_scriptCursor].atMs <= elapsed)
    {
        buttons = g_script[g_scriptCursor].buttons;
        ++g_scriptCursor;
        fired = true;
    }
    if (!fired)
        return;
    guard.unlock();

    PadState next;
    next.buttons = buttons;
    Publish(next);
    lucent::info("input", "scripted pad at {} ms: buttons {:#06x}", elapsed, buttons);
}

#ifdef GEARS_HAVE_PRESENTER

void PollHostInput()
{
    UpdateScriptedInput();
    if (!g_haveWindow)
        return;

    // A scripted run drives the pad itself; mixing the two would make the
    // script non-reproducible.
    if (!g_script.empty())
        return;

    if (!g_gamepad)
    {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids)
        {
            if (count > 0)
            {
                g_gamepad = SDL_OpenGamepad(ids[0]);
                if (g_gamepad)
                    lucent::info("input", "gamepad \"{}\" opened",
                        SDL_GetGamepadName(g_gamepad));
            }
            SDL_free(ids);
        }
    }

    PadState next;

    if (g_gamepad)
    {
        for (const PadAxisBinding& b : kPadBindings)
            if (SDL_GetGamepadButton(g_gamepad, b.button))
                next.buttons |= b.bit;
        // SDL reports triggers on the same 0..32767 axis range as the sticks;
        // the console's are a byte.
        auto trigger = [&](SDL_GamepadAxis axis) {
            const int value = SDL_GetGamepadAxis(g_gamepad, axis);
            return uint8_t(std::clamp(value, 0, 32767) * 255 / 32767);
        };
        next.leftTrigger = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        next.rightTrigger = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        next.thumbLX = int16_t(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX));
        // SDL's Y axis points down, the console's up.
        next.thumbLY = int16_t(-std::clamp<int>(
            SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY), -32767, 32767));
        next.thumbRX = int16_t(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
        next.thumbRY = int16_t(-std::clamp<int>(
            SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTY), -32767, 32767));
    }

    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (keys)
    {
        for (const KeyBinding& b : kKeyBindings)
            if (keys[b.key])
                next.buttons |= b.button;
        if (keys[SDL_SCANCODE_W]) next.thumbLY = kFullDeflection;
        if (keys[SDL_SCANCODE_S]) next.thumbLY = -kFullDeflection;
        if (keys[SDL_SCANCODE_A]) next.thumbLX = -kFullDeflection;
        if (keys[SDL_SCANCODE_D]) next.thumbLX = kFullDeflection;
        if (keys[SDL_SCANCODE_I]) next.thumbRY = kFullDeflection;
        if (keys[SDL_SCANCODE_K]) next.thumbRY = -kFullDeflection;
        if (keys[SDL_SCANCODE_J]) next.thumbRX = -kFullDeflection;
        if (keys[SDL_SCANCODE_L]) next.thumbRX = kFullDeflection;
        if (keys[SDL_SCANCODE_2]) next.leftTrigger = 255;
        if (keys[SDL_SCANCODE_4]) next.rightTrigger = 255;
    }

    Publish(next);
}

#else

void PollHostInput()
{
    UpdateScriptedInput();
}

#endif

} // namespace gears
