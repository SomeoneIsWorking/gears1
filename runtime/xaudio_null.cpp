// The XAudio render-driver surface, backed by a NULL audio device.
//
// As with the null GPU, this is not an audio implementation. It accepts
// registration and accepts submitted frames, and plays nothing. Frames are
// accepted rather than refused because a title whose submit fails will usually
// stall its audio thread, which would obscure everything downstream of it.
#include "import_stub.h"

#include <atomic>

#include <byteswap.h>
#include <lucent/log.h>

namespace
{
std::atomic<uint32_t> g_nextClientId{1};
std::atomic<uint64_t> g_submittedFrames{0};
uint32_t g_callback = 0;
uint32_t g_callbackContext = 0;
} // namespace

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

    // Nothing drives this callback: it would be called from an audio thread
    // asking for the next buffer, and there is no audio thread. A title that
    // waits to be asked will go quiet rather than crash.
    lucent::warn("audio", "NULL audio: client {} registered, callback {:#x} will never fire",
        clientId, g_callback);
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XAudioUnregisterRenderDriverClient(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::info("audio", "client unregistered after {} submitted frames",
        g_submittedFrames.load());
    ctx.r3.u64 = gears::kStatusSuccess;
}

void __imp__XAudioSubmitRenderDriverFrame(PPCContext& __restrict ctx, uint8_t*)
{
    const uint64_t frames = g_submittedFrames.fetch_add(1) + 1;
    if (frames == 1 || frames % 1000 == 0)
        lucent::info("audio", "{} frames submitted (nothing played)", frames);
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
