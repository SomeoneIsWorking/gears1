// The XAudio render-driver surface, backed by a NULL audio device.
//
// As with the null GPU, this is not an audio implementation. It accepts
// registration and accepts submitted frames, and plays nothing. Frames are
// accepted rather than refused because a title whose submit fails will usually
// stall its audio thread, which would obscure everything downstream of it.
#include "import_stub.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <sys/resource.h>

#include "byte_order.h"
#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_heap.h"
#include "guest_memory.h"
#include "audio_pace.h"
#include "audio_frame.h"
#include "audio_out.h"
#include "guest_thread.h"
#include "missing_x360port_executor.h"
#include "wait_probe.h"
#include "xma.h"

namespace
{
std::atomic<uint32_t> g_nextClientId{1};
std::atomic<uint64_t> g_submittedFrames{0};
std::atomic<uint64_t> g_nullSampleFrames{0};
std::atomic<uint64_t> g_silentFrames{0};
// Whether a device is actually consuming these frames, so the periodic line
// does not claim they are unplayed once they are being played.
std::atomic<bool> g_playing{false};
uint32_t g_callback = 0;
uint32_t g_callbackContext = 0;

// A submitted frame is 6 channels x 256 samples of big-endian float, laid out
// CHANNEL-MAJOR: all 256 samples of channel 0, then all of channel 1, and so on.
// That is the layout Xenia's converter reads (conversion.h, "sequential_6"), and
// it is not what a host audio API wants -- see runtime/audio_frame.h, which owns
// the conversion and the evidence for the layout.
constexpr uint32_t kFrameChannels = 6;
constexpr uint32_t kFrameSamplesPerChannel = 256;
constexpr uint32_t kFrameFloats = kFrameChannels * kFrameSamplesPerChannel;
constexpr uint32_t kFrameSampleRate = 48000;

float LoadGuestFloat(const uint8_t *at)
{
    uint32_t bits;
    std::memcpy(&bits, at, sizeof(bits));
    bits = ByteSwap(bits);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// The peak of the frame the title just handed us.
//
// This exists because "the title submitted 11250 frames" and "the title
// submitted 11250 frames of digital silence" are the same observation from the
// driver's side, and only one of them is audio working. Cheap enough to run
// unconditionally at 187.5 Hz.
float FramePeak(const uint8_t *samples)
{
    float peak = 0.0f;
    for (uint32_t i = 0; i < kFrameFloats; ++i)
    {
        const float value = LoadGuestFloat(samples + i * 4);
        // NaNs compare false, so they are not mistaken for a loud frame.
        const float magnitude = value < 0.0f ? -value : value;
        if (magnitude > peak)
            peak = magnitude;
    }
    return peak;
}

// GEARS_AUDIO_WAV=<path> writes what the title submits: 32-bit float, six
// channels, 48 kHz, deinterleaved into the order a WAV file is defined to have
// and otherwise untouched. No downmix, no gain, no resampling -- the point of the
// dump is to answer what the TITLE produced, so anything beyond making the
// container honest would be a second thing that can be wrong.
class WavWriter
{
  public:
    void Open(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path out(path);
        if (out.has_parent_path())
            std::filesystem::create_directories(out.parent_path(), ec);
        file_ = std::fopen(path.c_str(), "wb");
        if (!file_)
        {
            lucent::error("audio", "cannot open {} for the sample dump", path);
            return;
        }
        WriteHeader(0);
        lucent::info("audio", "writing submitted samples to {} ({} ch, {} Hz, f32)", path,
                     kFrameChannels, kFrameSampleRate);
    }

    void Write(const uint8_t *samples)
    {
        if (!file_)
            return;
        // Deinterleaved, because the file claims to be a six-channel WAV and a
        // WAV is interleaved. Writing the guest's planes into it byte for byte
        // would be a file that lies about its own format -- which is how this
        // dump managed to look like evidence while carrying the layout bug.
        float host[kFrameFloats];
        gears::DeinterleaveGuestAudioFrame(samples, host, kFrameChannels, kFrameSamplesPerChannel);
        std::fwrite(host, sizeof(float), kFrameFloats, file_);
        dataBytes_ += sizeof(host);

        // The header is refreshed about once a second of audio, because runs
        // end by being killed far more often than they end cleanly -- the
        // capture script sends SIGKILL, and atexit never sees it. A dump that
        // only becomes readable on a graceful exit is a dump that is unreadable
        // when it matters.
        if (++framesSinceHeader_ >= kHeaderRefreshFrames)
        {
            framesSinceHeader_ = 0;
            const long end = std::ftell(file_);
            std::fseek(file_, 0, SEEK_SET);
            WriteHeader(dataBytes_);
            std::fseek(file_, end, SEEK_SET);
        }
    }

    void Close()
    {
        if (!file_)
            return;
        std::fseek(file_, 0, SEEK_SET);
        WriteHeader(dataBytes_);
        std::fclose(file_);
        file_ = nullptr;
        lucent::info("audio", "sample dump closed: {} bytes of PCM ({:.1f} s)", dataBytes_,
                     double(dataBytes_) / (sizeof(float) * kFrameChannels * kFrameSampleRate));
    }

    bool open() const { return file_ != nullptr; }

  private:
    void WriteHeader(uint32_t dataBytes)
    {
        const uint32_t byteRate = kFrameSampleRate * kFrameChannels * sizeof(float);
        const uint16_t blockAlign = uint16_t(kFrameChannels * sizeof(float));
        auto u32 = [this](uint32_t v) { std::fwrite(&v, 4, 1, file_); };
        auto u16 = [this](uint16_t v) { std::fwrite(&v, 2, 1, file_); };
        std::fwrite("RIFF", 1, 4, file_);
        u32(36 + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, file_);
        u32(16);
        u16(3); // IEEE float
        u16(uint16_t(kFrameChannels));
        u32(kFrameSampleRate);
        u32(byteRate);
        u16(blockAlign);
        u16(32);
        std::fwrite("data", 1, 4, file_);
        u32(dataBytes);
    }

    static constexpr uint32_t kHeaderRefreshFrames = 188; // ~1 s at 187.5 Hz

    std::FILE *file_ = nullptr;
    uint32_t dataBytes_ = 0;
    uint32_t framesSinceHeader_ = 0;
};

WavWriter g_wav;
std::once_flag g_wavOpened;
std::mutex g_wavMutex;

// The audio pump.
//
// On hardware the render driver CALLS the title to ask for the next frame; the
// title then hands it back through XAudioSubmitRenderDriverFrame. Nothing here
// was making that call, so the title sat waiting to be asked and submitted
// exactly ZERO frames -- audio was silent because none was ever produced, not
// because it was produced and dropped (catalog #39).
//
// Rate is Xenia's AudioDriver contract: 48000 Hz over 256 samples per channel,
// so 187.5 frames per second. The callback takes one argument, the context
// captured at registration (Xenia: AudioSystem::WorkerThreadMain passes
// client_callback_arg alone).
//
// Calling the registered guest callback requires x360port to supply a Xenia
// ThreadState. The retained pacing and frame contracts do not implement that
// executor boundary themselves.
constexpr double kAudioFramesPerSecond = 48000.0 / 256.0;
std::atomic<bool> g_pumpStop{false};
std::thread g_pumpThread;
std::atomic<uint64_t> g_pumpCalls{0};

// The pump thread's CPU time, for separating two explanations of a slow pump
// that look identical from outside (catalog #43): a callback that is genuinely
// expensive burns thread CPU time roughly equal to its wall time; a callback
// that is preempted or blocked shows wall time far above CPU time. One number
// distinguishes "the mixer is slow" from "the mixer is starved", which decide
// for opposite fixes.
uint64_t PumpThreadCpuNanos()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// Context switches distinguish the two ways a thread loses wall time while
// burning no CPU: voluntary switches are waits it chose (a lock, a sleep --
// something the wait-site probes can name); involuntary switches are the
// scheduler taking the core away while the thread was RUNNABLE. Wall >> CPU
// with no named wait site and rising involuntary switches is preemption under
// load, which no amount of code inside the callback can fix.
void PumpThreadSwitches(uint64_t &voluntary, uint64_t &involuntary)
{
    rusage usage{};
    getrusage(RUSAGE_THREAD, &usage);
    voluntary = uint64_t(usage.ru_nvcsw);
    involuntary = uint64_t(usage.ru_nivcsw);
}

// The scheduler's own account of time this thread spent RUNNABLE but not
// running (/proc/thread-self/schedstat, second field, nanoseconds). This is
// the direct measurement of "the machine would not give the pump a core":
// if wall - cpu for an interval is roughly this delta, the pump is starved by
// scheduling and nothing inside the callback explains anything.
uint64_t PumpThreadRunqueueNanos()
{
    // Reopened per read: the file is per-thread and this runs 10 times a
    // second on a thread whose whole job is waiting.
    std::FILE *f = std::fopen("/proc/thread-self/schedstat", "r");
    if (!f)
        return 0;
    unsigned long long ran = 0, waited = 0, slices = 0;
    const int got = std::fscanf(f, "%llu %llu %llu", &ran, &waited, &slices);
    std::fclose(f);
    return got == 3 ? waited : 0;
}

void AudioPump()
{
    gears::GuestThreadBlock block{};
    if (!gears::CreateGuestThreadBlock(gears::Memory(), 0x10000, block))
    {
        lucent::error("audio", "audio pump: no guest thread block; the title will"
                               " never be asked for a frame");
        return;
    }
    gears::SetGuestThreadName("audio-pump");
    lucent::info("audio", "audio pump thread running (pcr {:#x}, stack {:#x})", block.pcrAddress,
                 block.stackBase);
    PPCContext ctx{};
    ctx.r13.u32 = block.pcrAddress;
    ctx.fpscr.loadFromHost();
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / kAudioFramesPerSecond));
    auto next = clock::now() + period;

    // Interval accumulators for the periodic report. Wall vs CPU time of the
    // callback is the load-bearing pair; the rest sizes the backlog.
    uint64_t wallNanos = 0, wallMaxNanos = 0;
    uint64_t cpuNanos = 0, cpuMaxNanos = 0;
    uint64_t lateSlots = 0, timedCalls = 0;
    // Slots given up because the pump was too far behind to serve them in time.
    // Counted and reported: a pump that silently skips reads exactly like a pump
    // that is keeping up.
    uint64_t droppedSlots = 0;
    // The wall time the reporting interval actually took, which is what says
    // whether the title is being asked for frames at 187.5 Hz. Nothing else in
    // the report answers that -- late slots and backlog say the pump is
    // struggling, not what rate came out of it.
    auto intervalStart = clock::now();

    while (!g_pumpStop.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_until(next);
        const auto due = next;
        const uint32_t callback = g_callback;
        if (!callback)
            continue;

        const auto wallBefore = clock::now();
        const uint64_t cpuBefore = PumpThreadCpuNanos();
        // Behind by a full slot or more even after sleeping: this iteration is
        // a catch-up run, not a paced one.
        if (wallBefore >= due + period)
            ++lateSlots;

        ctx.r1.u32 = block.stackBase - 0x100;
        ctx.r3.u32 = g_callbackContext;
        gears::t_inAudioPumpCallback = true;
        gears::RefuseMissingX360PortExecutor(callback);
        gears::t_inAudioPumpCallback = false;

        const uint64_t cpuDelta = PumpThreadCpuNanos() - cpuBefore;
        const uint64_t wallDelta =
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - wallBefore)
                         .count());
        wallNanos += wallDelta;
        cpuNanos += cpuDelta;
        wallMaxNanos = std::max(wallMaxNanos, wallDelta);
        cpuMaxNanos = std::max(cpuMaxNanos, cpuDelta);
        ++timedCalls;

        // WHERE THE NEXT SLOT GOES. Not `next += period` unconditionally: after a
        // long stall that schedules every missed slot in the past, so the loop
        // fires them back to back and the title races through its audio stream --
        // heard as a raised pitch for as long as the deficit lasts. The pacer
        // gives the missed slots up instead, and says how many. See
        // runtime/audio_pace.h and tests/test_audio_pace.cpp.
        {
            const auto nowNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      clock::now().time_since_epoch())
                                      .count();
            const auto dueNanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(due.time_since_epoch())
                    .count();
            const auto periodNanos =
                std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
            const gears::PaceDecision paced = gears::PaceAudioPump(nowNanos, dueNanos, periodNanos);
            droppedSlots += uint64_t(paced.droppedSlots);
            next = clock::time_point(std::chrono::duration_cast<clock::duration>(
                std::chrono::nanoseconds(paced.nextSlotNanos)));
        }

        const uint64_t n = g_pumpCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        // Report the pump separately from the submissions: "the callback is
        // being entered and returning" and "the title is producing frames" are
        // different facts, and only the first one is ours to guarantee.
        if (n == 1 || n % 1875 == 0)
        {
            const auto now = clock::now();
            const double backlogSlots =
                now < next
                    ? 0.0
                    : std::chrono::duration<double>(now - next).count() * kAudioFramesPerSecond;
            static uint64_t lastVoluntary = 0, lastInvoluntary = 0;
            static uint64_t lastRunqueue = 0;
            uint64_t voluntary = 0, involuntary = 0;
            PumpThreadSwitches(voluntary, involuntary);
            const uint64_t runqueue = PumpThreadRunqueueNanos();
            const double intervalSeconds =
                std::chrono::duration<double>(now - intervalStart).count();
            intervalStart = now;
            lucent::info("audio",
                         "pump: {} callback invocations, {} frames"
                         " submitted by the title; last {} in {:.2f}s = {:.1f} Hz against"
                         " 187.5 ({} slots dropped): wall {}/{} us mean/max,"
                         " cpu {}/{} us mean/max, {} late slots, backlog {:.0f} slots,"
                         " csw {} vol {} invol, runqueue {} ms",
                         n, g_submittedFrames.load(), timedCalls, intervalSeconds,
                         intervalSeconds > 0 ? double(timedCalls) / intervalSeconds : 0.0,
                         droppedSlots, timedCalls ? wallNanos / timedCalls / 1000 : 0,
                         wallMaxNanos / 1000, timedCalls ? cpuNanos / timedCalls / 1000 : 0,
                         cpuMaxNanos / 1000, lateSlots, backlogSlots, voluntary - lastVoluntary,
                         involuntary - lastInvoluntary, (runqueue - lastRunqueue) / 1000000);
            lastVoluntary = voluntary;
            lastInvoluntary = involuntary;
            lastRunqueue = runqueue;
            droppedSlots = 0;
            wallNanos = wallMaxNanos = cpuNanos = cpuMaxNanos = 0;
            lateSlots = timedCalls = 0;
            gears::WaitProbeReport();
        }
    }
}
} // namespace

namespace gears
{
void StopAudioPump()
{
    g_pumpStop.store(true);
    if (g_pumpThread.joinable())
        g_pumpThread.join();
}
} // namespace gears

// NTSTATUS XAudioRegisterRenderDriverClient(PDWORD Callback, PDWORD ClientId)
void __imp__XAudioRegisterRenderDriverClient(PPCContext &__restrict ctx, uint8_t *base)
{
    const uint32_t callbackPtr = ctx.r3.u32;
    const uint32_t clientIdPtr = ctx.r4.u32;

    if (callbackPtr != 0)
    {
        g_callback = ByteSwap(*reinterpret_cast<uint32_t *>(base + callbackPtr));
        g_callbackContext = ByteSwap(*reinterpret_cast<uint32_t *>(base + callbackPtr + 4));
    }

    // The driver handle the title gets back is 0x41550000 | index -- an 'AU'
    // magic in the top half (Xenia: XAudioRegisterRenderDriverClient_entry, and
    // XAudioSubmitRenderDriverFrame asserts on it). We were handing back a bare
    // 1, so any title that checks or unpacks the handle sees a value the console
    // would never produce.
    const uint32_t index = g_nextClientId.fetch_add(1);
    const uint32_t clientId = 0x41550000u | (index & 0xFFFFu);
    if (clientIdPtr != 0)
        *reinterpret_cast<uint32_t *>(base + clientIdPtr) = ByteSwap(clientId);

    // ON BY DEFAULT now. It was opt-in while driving the callback turned a
    // stable silent title into a crashing one, and then into a stalled one;
    // both causes are fixed and the whole chain -- pump, callback, XMA decode,
    // submitted frame -- is verified. A port that has working audio and does
    // not ask for it is just silent on purpose. GEARS_AUDIO_PUMP=0 turns it off.
    //
    // It costs about 4% of frame rate under a CPU-bound guest (15.9 fps against
    // 16.6 in a control with no pump), which is the price of the title actually
    // mixing its audio.
    const bool pumpWanted =
        !lucent::config::present("AUDIO_PUMP") || lucent::config::flag("AUDIO_PUMP");
    if (pumpWanted && g_callback && !g_pumpThread.joinable())
    {
        g_pumpThread = std::thread(AudioPump);
        lucent::info("audio",
                     "client {} registered; pumping callback {:#x} at"
                     " {:.1f} Hz",
                     clientId, g_callback, kAudioFramesPerSecond);
    }
    else
    {
        lucent::warn("audio",
                     "client {} registered, callback {:#x} NOT driven"
                     " (GEARS_AUDIO_PUMP=1 drives it) -- the title will submit no frames",
                     clientId, g_callback);
    }
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XAudioUnregisterRenderDriverClient(PPCContext &__restrict ctx, uint8_t *)
{
    lucent::info("audio", "client unregistered after {} submitted frames",
                 g_submittedFrames.load());
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS XAudioSubmitRenderDriverFrame(DWORD ClientId, void* SampleBuffer)
//
// r4 is the SAMPLES -- a float* of 6 channels x 256 samples interleaved (Xenia:
// xboxkrnl_audio.cc). It used to be discarded, which would have thrown the audio
// away even once the pump started asking for it.
void __imp__XAudioSubmitRenderDriverFrame(PPCContext &__restrict ctx, uint8_t *base)
{
    const uint32_t samplesPtr = ctx.r4.u32;
    const uint64_t frames = g_submittedFrames.fetch_add(1) + 1;
    // The title mixing a frame is audio progress. Watched separately from
    // drawing because they have been seen to stop independently.
    gears::NoteGuestProgress("audio");

    float peak = 0.0f;
    if (samplesPtr == 0)
    {
        g_nullSampleFrames.fetch_add(1);
    }
    else
    {
        const uint8_t *samples = base + samplesPtr;
        peak = FramePeak(samples);
        // Exact zero, not a threshold: the question here is whether the title
        // wrote anything at all, and a quiet frame is a different finding from
        // an untouched buffer.
        if (peak == 0.0f)
            g_silentFrames.fetch_add(1);

        // Play it. Opened lazily on the first frame that has samples, using
        // the geometry the frame actually has rather than a guess made at
        // startup. GEARS_AUDIO_OUT=0 turns it off for a run that only wants to
        // measure the mix.
        // Playing the audio is the point, so this is on by default -- but NOT
        // in a headless run. A capture or a scripted walk is measurement, and
        // measurement that unexpectedly makes noise out of the operator's
        // speakers is a bad neighbour; GEARS_NO_WINDOW already means "this run
        // is not a presentation". GEARS_AUDIO_OUT=1 forces it on anyway, and
        // GEARS_AUDIO_OUT=0 forces it off. `present` is what separates "not
        // set" from "set to 0".
        // A run makes noise only when it is a PRESENTATION -- a real window that a
        // person is watching. GEARS_NO_WINDOW is one way to say "this is
        // measurement"; GEARS_PRESENT_HEADLESS is another, and it was missed. That
        // mode runs the whole present path against a headless surface, so it has
        // every property of a measurement run except the name, and it played sound
        // out of the operator's speakers in the middle of their work.
        const bool measurementRun =
            lucent::config::flag("NO_WINDOW") || lucent::config::flag("PRESENT_HEADLESS");
        const bool audioOutDefault = !measurementRun;
        if (lucent::config::present("AUDIO_OUT") ? lucent::config::flag("AUDIO_OUT")
                                                 : audioOutDefault)
        {
            static std::once_flag opened;
            std::call_once(opened,
                           []
                           {
                               if (gears::OpenAudioOutput(kFrameChannels, kFrameSampleRate))
                                   std::atexit(gears::CloseAudioOutput);
                           });
            float host[kFrameFloats];
            gears::DeinterleaveGuestAudioFrame(samples, host, kFrameChannels,
                                               kFrameSamplesPerChannel);
            gears::PlayAudioFrame(host, kFrameSamplesPerChannel);
            g_playing = true;
        }

        if (const std::string &path = lucent::config::text("AUDIO_WAV"); !path.empty())
        {
            std::call_once(g_wavOpened,
                           [&path]
                           {
                               g_wav.Open(path);
                               if (g_wav.open())
                                   std::atexit([] { g_wav.Close(); });
                           });
            std::lock_guard<std::mutex> guard(g_wavMutex);
            g_wav.Write(samples);
        }
    }

    if (frames == 1 || frames % 1000 == 0)
        lucent::info("audio",
                     "{} frames submitted ({} pump calls, samples at"
                     " {:#x}, {} with no buffer, {} silent), peak {:.4f}{}",
                     frames, g_pumpCalls.load(), samplesPtr, g_nullSampleFrames.load(),
                     g_silentFrames.load(), peak,
                     g_playing ? "" : " -- not played, no device on this run");
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XAudioGetVoiceCategoryVolume(PPCContext &__restrict ctx, uint8_t *base)
{
    // 1.0f, i.e. unattenuated.
    if (ctx.r4.u32 != 0)
        *reinterpret_cast<uint32_t *>(base + ctx.r4.u32) = ByteSwap(0x3F800000u);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XAudioGetVoiceCategoryVolumeChangeMask(PPCContext &__restrict ctx, uint8_t *base)
{
    // No volume has changed, because nothing can change one.
    if (ctx.r4.u32 != 0)
        *reinterpret_cast<uint32_t *>(base + ctx.r4.u32) = 0;
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS XMACreateContext(PVOID* OutContext)
//
// The XMA block is the console's hardware audio decoder. Refusing context
// creation was measured to be intolerable: the title's audio device reports
// a successful init anyway and then crashes dispatching through the members
// it never built (SIGSEGV in its Update path). So creation succeeds and
// hands out a real, zeroed context record in physical memory -- the title
// programs it and kicks the decoder block through MMIO, where xma_context.cpp
// decodes it (verified against an independent reference, catalog #43's
// neighbours).
void __imp__XMACreateContext(PPCContext &__restrict ctx, uint8_t *base)
{
    static std::atomic<uint64_t> s_created{0};

    // A slot in the hardware's context array, not a fresh allocation. The title
    // identifies a context by its INDEX in that array -- the kick, lock and
    // clear registers are bitmaps over those indices -- so a context that lives
    // anywhere else cannot be named by the interface that drives it (xma.h).
    const uint32_t context = gears::AllocateXmaContext();
    if (context == 0)
    {
        ctx.r3.u64 = gears::kStatusNoMemory;
        return;
    }

    if (ctx.r3.u32 != 0)
        *reinterpret_cast<uint32_t *>(base + ctx.r3.u32) = ByteSwap(context);

    const uint64_t n = s_created.fetch_add(1) + 1;
    lucent::debug("audio", "XMACreateContext -> {:#x} ({} live)", context, n);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XMAReleaseContext(PPCContext &__restrict ctx, uint8_t *)
{
    gears::ReleaseXmaContext(ctx.r3.u32);
    ctx.r3.u64 = 0;
}
