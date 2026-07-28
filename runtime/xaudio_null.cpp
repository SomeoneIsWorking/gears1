// The XAudio render-driver surface, backed by a NULL audio device.
//
// As with the null GPU, this is not an audio implementation. It accepts
// registration and accepts submitted frames, and plays nothing. Frames are
// accepted rather than refused because a title whose submit fails will usually
// stall its audio thread, which would obscure everything downstream of it.
#include "import_stub.h"

#include <atomic>
#include <chrono>
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
uint32_t g_callback = 0;
uint32_t g_callbackContext = 0;

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

    const uint32_t clientId = g_nextClientId.fetch_add(1);
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
void __imp__XAudioSubmitRenderDriverFrame(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t samplesPtr = ctx.r4.u32;
    const uint64_t frames = g_submittedFrames.fetch_add(1) + 1;
    if (samplesPtr == 0)
        g_nullSampleFrames.fetch_add(1);
    if (frames == 1 || frames % 1000 == 0)
        lucent::info("audio", "{} frames submitted ({} pump calls, samples at"
            " {:#x}, {} with no buffer) -- not played yet", frames,
            g_pumpCalls.load(), samplesPtr, g_nullSampleFrames.load());
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
