// The Xenos video driver surface, backed by a NULL GPU.
//
// This is not a graphics implementation and does not pretend to be one. It
// tracks the state the driver genuinely owns -- ring buffer location, write-back
// pointer, display mode -- and then consumes submitted command buffers without
// executing them. Nothing is rasterised and nothing is presented.
//
// Consuming is not a lie: it models a GPU that retires work instantly. The
// alternative, never advancing the read pointer, would deadlock the guest
// against a full ring buffer and tell us nothing. What IS missing is the
// command processor that would interpret those packets, and every function here
// that would need one says so.
#include "import_stub.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_heap.h"
#include "guest_thread.h"
#include "guest_memory.h"

PPC_EXTERN_FUNC(__imp__XGetVideoMode);

namespace
{

struct RingBuffer
{
    uint32_t base;
    uint32_t sizeLog2;
    uint32_t readPtrWriteBackAddress;
    uint32_t readPtrWriteBackBlockSize;
};

RingBuffer g_ringBuffer{};
uint32_t g_graphicsInterruptCallback = 0;
uint32_t g_graphicsInterruptContext = 0;
uint32_t g_systemCommandBufferGpuIdentifier = 0;
std::atomic<uint64_t> g_frameCount{0};

void StoreGuest32(uint32_t address, uint32_t value)
{
    if (address != 0)
        *gears::Memory().Translate<uint32_t>(address) = ByteSwap(value);
}

// Reports the whole ring buffer as consumed. See the file comment: this is the
// instant-retirement model, not command execution.
void RetireRingBuffer()
{
    if (g_ringBuffer.readPtrWriteBackAddress == 0)
        return;

    const uint32_t writePtr = 1u << g_ringBuffer.sizeLog2;
    StoreGuest32(g_ringBuffer.readPtrWriteBackAddress, writePtr);
}

} // namespace

namespace gears
{

// The console's memory-mapped device window. Guest code reaches it through the
// MMIO macros and through byte-reversed loads and stores (`lwbrx`/`stwbrx`,
// because device registers are little-endian), never through a Vd* call. The
// Xenos register file at 0x7FC80000 lives here, along with other device blocks.
//
// The whole window is committed as inert memory rather than page-by-page as
// each new block is discovered: no devices are modelled, so every register in
// it behaves the same way -- reads see back what was written. Committing it as
// one region is a model; committing addresses individually as they fault would
// just be chasing symptoms.
bool CommitDeviceWindow(GuestMemory& memory)
{
    constexpr uint32_t kDeviceWindowBase = 0x7FC00000;
    constexpr uint32_t kDeviceWindowSize = 0x00400000;

    if (!memory.Commit(kDeviceWindowBase, kDeviceWindowSize))
        return false;

    lucent::info("gpu", "device MMIO window {:#x}..{:#x} committed (inert)",
        kDeviceWindowBase, kDeviceWindowBase + kDeviceWindowSize);
    return true;
}

} // namespace gears

void __imp__VdInitializeEngines(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::warn("gpu", "VdInitializeEngines -- NULL GPU: no commands will be executed");
    ctx.r3.u64 = 1;
}

void __imp__VdShutdownEngines(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::info("gpu", "VdShutdownEngines after {} submitted frames", g_frameCount.load());
    ctx.r3.u64 = 0;
}

void __imp__VdInitializeRingBuffer(PPCContext& __restrict ctx, uint8_t*)
{
    g_ringBuffer.base = ctx.r3.u32;
    g_ringBuffer.sizeLog2 = ctx.r4.u32;
    lucent::info("gpu", "ring buffer at {:#x}, {} bytes", g_ringBuffer.base, 1u << g_ringBuffer.sizeLog2);
    ctx.r3.u64 = 0;
}

void __imp__VdEnableRingBufferRPtrWriteBack(PPCContext& __restrict ctx, uint8_t*)
{
    g_ringBuffer.readPtrWriteBackAddress = ctx.r3.u32;
    g_ringBuffer.readPtrWriteBackBlockSize = ctx.r4.u32;
    lucent::info("gpu", "ring buffer read-pointer write-back at {:#x}",
        g_ringBuffer.readPtrWriteBackAddress);
    RetireRingBuffer();
    ctx.r3.u64 = 0;
}

void __imp__VdSetSystemCommandBufferGpuIdentifierAddress(PPCContext& __restrict ctx, uint8_t*)
{
    g_systemCommandBufferGpuIdentifier = ctx.r3.u32;
    ctx.r3.u64 = 0;
}

namespace
{
// The console raises this from the GPU at vblank and on command-buffer
// completion, and titles advance real state machines from it. Not raising it at
// all leaves those state machines frozen, which is not a neutral omission -- it
// is its own kind of wrong. So it is driven from a host thread at the display
// refresh rate.
//
// This is still not a command processor: the callback reports vblank, never
// actual completion of work, because no work is executed. Set
// GEARS_NO_VBLANK=1 to disable it and get the old never-fires behaviour, which
// is useful for telling the two failure modes apart.
void VblankThread()
{
    gears::GuestThreadBlock block{};
    if (!gears::CreateGuestThreadBlock(gears::Memory(), 0x10000, block))
    {
        lucent::error("gpu", "cannot create the vblank thread's guest block");
        return;
    }

    uint8_t* base = gears::Memory().Base();
    PPCContext ctx{};
    ctx.r13.u32 = block.pcrAddress;
    ctx.fpscr.loadFromHost();

    lucent::info("gpu", "vblank thread driving interrupt callback {:#x} at 60 Hz",
        g_graphicsInterruptCallback);

    while (true)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(16667));

        ctx.r1.u32 = block.stackBase - 0x100;
        ctx.r3.u32 = 0; // source: vblank
        ctx.r4.u32 = g_graphicsInterruptContext;
        (PPC_LOOKUP_FUNC(base, g_graphicsInterruptCallback))(ctx, base);
    }
}
} // namespace

void __imp__VdSetGraphicsInterruptCallback(PPCContext& __restrict ctx, uint8_t*)
{
    g_graphicsInterruptCallback = ctx.r3.u32;
    g_graphicsInterruptContext = ctx.r4.u32;

    if (getenv("GEARS_NO_VBLANK") != nullptr)
    {
        lucent::warn("gpu", "GEARS_NO_VBLANK set: interrupt callback {:#x} will never fire",
            g_graphicsInterruptCallback);
    }
    else if (g_graphicsInterruptCallback != 0)
    {
        std::thread(VblankThread).detach();
    }

    ctx.r3.u64 = 0;
}

void __imp__VdSwap(PPCContext& __restrict ctx, uint8_t*)
{
    const uint64_t frame = g_frameCount.fetch_add(1) + 1;
    if (frame == 1 || frame % 60 == 0)
        lucent::info("gpu", "VdSwap: {} frames submitted (nothing presented)", frame);

    RetireRingBuffer();
    ctx.r3.u64 = 0;
}

void __imp__VdQueryVideoFlags(PPCContext& __restrict ctx, uint8_t*)
{
    // Widescreen | HD, matching the 1280x720 mode XGetVideoMode reports.
    ctx.r3.u64 = 0x00000006;
}

void __imp__VdGetCurrentDisplayGamma(PPCContext& __restrict ctx, uint8_t*)
{
    StoreGuest32(ctx.r3.u32, 2);
    StoreGuest32(ctx.r4.u32, 0x40000000); // 2.0f
    ctx.r3.u64 = 0;
}

void __imp__VdGetCurrentDisplayInformation(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t p = ctx.r3.u32;
    if (p == 0)
        return;

    // Only the fields the title reads are filled; the rest stays zero so an
    // unexpected read shows up as a zero rather than as plausible noise.
    StoreGuest32(p + 0x00, (720u << 16) | 1280u); // height:width
    StoreGuest32(p + 0x08, 1280);
    StoreGuest32(p + 0x0C, 720);
    StoreGuest32(p + 0x14, 1280);
    StoreGuest32(p + 0x18, 720);
    StoreGuest32(p + 0x30, 1280);
    StoreGuest32(p + 0x34, 720);
    ctx.r3.u64 = 0;
}

void __imp__VdSetDisplayMode(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("gpu", "VdSetDisplayMode({:#x})", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

void __imp__VdIsHSIOTrainingSucceeded(PPCContext& __restrict ctx, uint8_t*)
{
    // The high-speed IO link between CPU and GPU. There is no link to train.
    ctx.r3.u64 = 1;
}

void __imp__VdPersistDisplay(PPCContext& __restrict ctx, uint8_t*)
{
    StoreGuest32(ctx.r4.u32, 0);
    ctx.r3.u64 = 1;
}

void __imp__VdRetrainEDRAM(PPCContext& __restrict ctx, uint8_t*)
{
    // EDRAM is physical memory on the console's GPU daughter die; there is no
    // equivalent here and nothing to retrain.
    ctx.r3.u64 = 0;
}

void __imp__VdRetrainEDRAMWorker(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

void __imp__VdEnableDisableClockGating(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

void __imp__VdCallGraphicsNotificationRoutines(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = 0;
}

void __imp__VdQueryVideoMode(PPCContext& __restrict ctx, uint8_t* base)
{
    // Same mode XGetVideoMode reports; the two must not disagree.
    __imp__XGetVideoMode(ctx, base);
    ctx.r3.u64 = 0;
}

// The system command buffer is a small ring the driver writes into directly.
// It is handed out as real, committed memory so the guest's writes land
// somewhere valid -- but as with the main ring buffer, nothing interprets what
// it writes.
void __imp__VdGetSystemCommandBuffer(PPCContext& __restrict ctx, uint8_t*)
{
    static uint32_t s_commandBuffer = 0;
    if (s_commandBuffer == 0)
    {
        uint32_t size = 0x10000;
        s_commandBuffer = gears::PhysicalHeap().Allocate(
            0, size, gears::kMemCommit | gears::kMemLargePages);
        lucent::info("gpu", "system command buffer at {:#x} (inert)", s_commandBuffer);
    }

    StoreGuest32(ctx.r3.u32, s_commandBuffer);
    StoreGuest32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

void __imp__VdInitializeScalerCommandBuffer(PPCContext& __restrict ctx, uint8_t*)
{
    // Returns the number of command words written. Writing none is truthful.
    ctx.r3.u64 = 0;
}
