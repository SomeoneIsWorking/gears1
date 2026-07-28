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
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <byteswap.h>
#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_heap.h"
#include "guest_memory.h"
#include "guest_thread.h"

namespace
{
std::atomic<uint32_t> g_nextClientId{1};
std::atomic<uint64_t> g_submittedFrames{0};
std::atomic<uint64_t> g_nullSampleFrames{0};
std::atomic<uint64_t> g_silentFrames{0};
uint32_t g_callback = 0;
uint32_t g_callbackContext = 0;

// A submitted frame is 6 channels x 256 samples of interleaved big-endian
// float, which is Xenia's XAudioSubmitRenderDriverFrame contract.
constexpr uint32_t kFrameChannels = 6;
constexpr uint32_t kFrameSamplesPerChannel = 256;
constexpr uint32_t kFrameFloats = kFrameChannels * kFrameSamplesPerChannel;
constexpr uint32_t kFrameSampleRate = 48000;

float LoadGuestFloat(const uint8_t* at)
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
float FramePeak(const uint8_t* samples)
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

// GEARS_AUDIO_WAV=<path> writes what the title submits, verbatim: 32-bit float,
// six channels, 48 kHz. Verbatim matters -- a downmix or a conversion here would
// be a second thing that can be wrong when the question is whether the FIRST
// thing produced anything.
class WavWriter
{
public:
    void Open(const std::string& path)
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
        lucent::info("audio", "writing submitted samples to {} ({} ch, {} Hz, f32)",
                     path, kFrameChannels, kFrameSampleRate);
    }

    void Write(const uint8_t* samples)
    {
        if (!file_)
            return;
        float host[kFrameFloats];
        for (uint32_t i = 0; i < kFrameFloats; ++i)
            host[i] = LoadGuestFloat(samples + i * 4);
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
        lucent::info("audio", "sample dump closed: {} bytes of PCM ({:.1f} s)",
                     dataBytes_,
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

    std::FILE* file_ = nullptr;
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
// Calling guest code from a host thread is the same mechanism the graphics ISR
// already uses -- a GuestThreadBlock for the PCR and stack, a PPCContext, then
// PPC_LOOKUP_FUNC.
constexpr double kAudioFramesPerSecond = 48000.0 / 256.0;
std::atomic<bool> g_pumpStop{false};
std::thread g_pumpThread;
std::atomic<uint64_t> g_pumpCalls{0};

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
    lucent::info("audio", "audio pump thread running (pcr {:#x}, stack {:#x})",
                 block.pcrAddress, block.stackBase);
    PPCContext ctx{};
    ctx.r13.u32 = block.pcrAddress;
    ctx.fpscr.loadFromHost();
    uint8_t* base = gears::Memory().Base();

    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / kAudioFramesPerSecond));
    auto next = clock::now() + period;
    while (!g_pumpStop.load(std::memory_order_relaxed))
    {
        std::this_thread::sleep_until(next);
        next += period;
        const uint32_t callback = g_callback;
        if (!callback)
            continue;
        ctx.r1.u32 = block.stackBase - 0x100;
        ctx.r3.u32 = g_callbackContext;
        (PPC_LOOKUP_FUNC(base, callback))(ctx, base);
        const uint64_t n = g_pumpCalls.fetch_add(1, std::memory_order_relaxed) + 1;
        // Report the pump separately from the submissions: "the callback is
        // being entered and returning" and "the title is producing frames" are
        // different facts, and only the first one is ours to guarantee.
        if (n == 1 || n % 1875 == 0)
            lucent::info("audio", "pump: {} callback invocations, {} frames"
                " submitted by the title", n, g_submittedFrames.load());
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
void __imp__XAudioRegisterRenderDriverClient(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t callbackPtr = ctx.r3.u32;
    const uint32_t clientIdPtr = ctx.r4.u32;

    if (callbackPtr != 0)
    {
        g_callback = ByteSwap(*reinterpret_cast<uint32_t*>(base + callbackPtr));
        g_callbackContext = ByteSwap(*reinterpret_cast<uint32_t*>(base + callbackPtr + 4));
    }

    // The driver handle the title gets back is 0x41550000 | index -- an 'AU'
    // magic in the top half (Xenia: XAudioRegisterRenderDriverClient_entry, and
    // XAudioSubmitRenderDriverFrame asserts on it). We were handing back a bare
    // 1, so any title that checks or unpacks the handle sees a value the console
    // would never produce.
    const uint32_t index = g_nextClientId.fetch_add(1);
    const uint32_t clientId = 0x41550000u | (index & 0xFFFFu);
    if (clientIdPtr != 0)
        *reinterpret_cast<uint32_t*>(base + clientIdPtr) = ByteSwap(clientId);

    // OFF BY DEFAULT (GEARS_AUDIO_PUMP=1 enables it), because it works and that
    // is the problem: the guest's audio callback immediately calls
    // KeWaitForMultipleObjects, which is unimplemented and aborts. Driving the
    // callback therefore turns a stable silent title into a crashing one. It
    // stays here, off, so the next step has a one-line repro (catalog #40).
    if (lucent::config::flag("AUDIO_PUMP") && g_callback &&
        !g_pumpThread.joinable())
    {
        g_pumpThread = std::thread(AudioPump);
        lucent::info("audio", "client {} registered; pumping callback {:#x} at"
            " {:.1f} Hz", clientId, g_callback, kAudioFramesPerSecond);
    }
    else
    {
        lucent::warn("audio", "client {} registered, callback {:#x} NOT driven"
            " (GEARS_AUDIO_PUMP=1 drives it) -- the title will submit no frames",
            clientId, g_callback);
    }
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XAudioUnregisterRenderDriverClient(PPCContext& __restrict ctx, uint8_t*)
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
void __imp__XAudioSubmitRenderDriverFrame(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t samplesPtr = ctx.r4.u32;
    const uint64_t frames = g_submittedFrames.fetch_add(1) + 1;

    float peak = 0.0f;
    if (samplesPtr == 0)
    {
        g_nullSampleFrames.fetch_add(1);
    }
    else
    {
        const uint8_t* samples = base + samplesPtr;
        peak = FramePeak(samples);
        // Exact zero, not a threshold: the question here is whether the title
        // wrote anything at all, and a quiet frame is a different finding from
        // an untouched buffer.
        if (peak == 0.0f)
            g_silentFrames.fetch_add(1);

        if (const std::string& path = lucent::config::text("AUDIO_WAV"); !path.empty())
        {
            std::call_once(g_wavOpened, [&path] {
                g_wav.Open(path);
                if (g_wav.open())
                    std::atexit([] { g_wav.Close(); });
            });
            std::lock_guard<std::mutex> guard(g_wavMutex);
            g_wav.Write(samples);
        }
    }

    if (frames == 1 || frames % 1000 == 0)
        lucent::info("audio", "{} frames submitted ({} pump calls, samples at"
            " {:#x}, {} with no buffer, {} silent), peak {:.4f} -- not played yet",
            frames, g_pumpCalls.load(), samplesPtr, g_nullSampleFrames.load(),
            g_silentFrames.load(), peak);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XAudioGetVoiceCategoryVolume(PPCContext& __restrict ctx, uint8_t* base)
{
    // 1.0f, i.e. unattenuated.
    if (ctx.r4.u32 != 0)
        *reinterpret_cast<uint32_t*>(base + ctx.r4.u32) = ByteSwap(0x3F800000u);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XAudioGetVoiceCategoryVolumeChangeMask(PPCContext& __restrict ctx, uint8_t* base)
{
    // No volume has changed, because nothing can change one.
    if (ctx.r4.u32 != 0)
        *reinterpret_cast<uint32_t*>(base + ctx.r4.u32) = 0;
    ctx.r3.u64 = gears::kStatusSuccess;
}

// NTSTATUS XMACreateContext(PVOID* OutContext)
//
// The XMA block is the console's hardware audio decoder. Refusing context
// creation was measured to be intolerable: the title's audio device reports
// a successful init anyway and then crashes dispatching through the members
// it never built (SIGSEGV in its Update path). So creation succeeds and
// hands out a real, zeroed context record in physical memory -- the title
// programs it and kicks the (unmodelled) decoder block through MMIO. What is
// missing is decode itself: no PCM is ever produced. That gap is logged
// loudly here and tracked as the audio subsystem's frontier.
void __imp__XMACreateContext(PPCContext& __restrict ctx, uint8_t* base)
{
    constexpr uint32_t kContextSize = 0x40; // the hardware context record
    static std::atomic<uint64_t> s_created{0};

    uint32_t size = kContextSize;
    const uint32_t context = gears::PhysicalHeap().Allocate(0, size, gears::kMemCommit);
    if (context == 0)
    {
        ctx.r3.u64 = gears::kStatusNoMemory;
        return;
    }

    if (ctx.r3.u32 != 0)
        *reinterpret_cast<uint32_t*>(base + ctx.r3.u32) = ByteSwap(context);

    const uint64_t n = s_created.fetch_add(1) + 1;
    if (n == 1)
        lucent::warn("audio", "XMA contexts handed out with NO decoder behind them"
            " -- audio will be silent and decode never progresses");
    lucent::debug("audio", "XMACreateContext -> {:#x} ({} live)", context, n);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XMAReleaseContext(PPCContext& __restrict ctx, uint8_t*)
{
    gears::PhysicalHeap().Free(ctx.r3.u32);
    ctx.r3.u64 = 0;
}
