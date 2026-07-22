// The Xenos video driver surface, backed by a minimal command processor.
//
// This is not a graphics implementation. Nothing is rasterised and nothing is
// presented. What it does execute -- because the title's D3D layer cannot make
// progress without them -- are the PM4 packets that carry the driver protocol:
//
//   TYPE0            register writes, including the scratch write-back
//                    mechanism (SCRATCH_UMSK/SCRATCH_ADDR): a value written to
//                    SCRATCH_REGn is copied by the GPU to SCRATCH_ADDR + 4n.
//   INDIRECT_BUFFER  the ring mostly contains pointers to command buffers.
//   EVENT_WRITE_SHD  the GPU writes a fence value to memory. This is what
//                    advances the "served" ticket the D3D lock at 0x82221A68
//                    waits on (observed: address 0x30A000|endian2, value = the
//                    submission's ticket from pool+0x2A1C).
//   WAIT_REG_MEM     the GPU polls memory/registers; the title uses it to
//                    order scratch write-backs against the interrupt.
//   INTERRUPT        raises the graphics interrupt with source 1 on the CPUs
//                    in the packet's mask; the title's ISR (0x82221C60) then
//                    calls the callback the stream placed in SCRATCH_REG4.
//   MEM_WRITE        plain memory writes from the stream.
//
// Everything else (draws, state, shaders) is skipped by count.
//
// Submission is the same as on hardware: the title stores the ring write
// pointer to the CP_RB_WPTR register (MMIO 0x7FC80714, one store site in the
// image at 0x82221424). The device window is committed memory, so the store
// lands there and the command processor thread polls it.
#include "import_stub.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
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
    uint32_t sizeLog2; // log2 of the size in QUADWORDS: bytes = 8 << sizeLog2.
                       // Verified live: sizeLog2=12 and the title masks ring
                       // dword indices with 0x1FFF (= 0x8000 bytes - 1 dword).
    uint32_t readPtrWriteBackAddress;
    uint32_t readPtrWriteBackBlockSize;

    uint32_t Bytes() const { return 8u << sizeLog2; }
    uint32_t Dwords() const { return 2u << sizeLog2; }
};

RingBuffer g_ringBuffer{};
uint32_t g_pm4WatchAddress = 0;
uint32_t g_graphicsInterruptCallback = 0;
uint32_t g_graphicsInterruptContext = 0;
uint32_t g_systemCommandBufferGpuIdentifier = 0;
std::atomic<uint64_t> g_frameCount{0};

uint32_t ReadGuest32(uint32_t address)
{
    return ByteSwap(*gears::Memory().Translate<uint32_t>(address));
}

void StoreGuest32(uint32_t address, uint32_t value)
{
    if (address != 0)
        *gears::Memory().Translate<uint32_t>(address) = ByteSwap(value);
}

// ---------------------------------------------------------------------------
// Interrupt dispatch.
//
// Both the vblank thread and the command processor call the title's graphics
// interrupt callback. On the console the two sources arrive on the same
// interrupt line and never preempt each other, so dispatch is serialised here
// too. The ISR reads its CPU number from KPCR+0x10C to clear its bit in the
// pending mask the stream set (SCRATCH_REG0), so the dispatching thread's PCR
// is stamped with the CPU the interrupt is addressed to.

std::mutex g_interruptMutex;
constexpr uint32_t kPcrCpuNumber = 0x10C;

struct InterruptThreadState
{
    gears::GuestThreadBlock block{};
    PPCContext ctx{};
    bool ready = false;

    bool Init()
    {
        if (!gears::CreateGuestThreadBlock(gears::Memory(), 0x10000, block))
            return false;
        ctx.r13.u32 = block.pcrAddress;
        ctx.fpscr.loadFromHost();
        ready = true;
        return true;
    }

    void Dispatch(uint32_t source, uint32_t cpu)
    {
        if (!ready || g_graphicsInterruptCallback == 0)
            return;
        std::lock_guard<std::mutex> lock(g_interruptMutex);
        uint8_t* base = gears::Memory().Base();
        *gears::Memory().Translate<uint8_t>(block.pcrAddress + kPcrCpuNumber) =
            uint8_t(cpu);
        ctx.r1.u32 = block.stackBase - 0x100;
        ctx.r3.u32 = source;
        ctx.r4.u32 = g_graphicsInterruptContext;
        (PPC_LOOKUP_FUNC(base, g_graphicsInterruptCallback))(ctx, base);
    }
};

// ---------------------------------------------------------------------------
// PM4 execution.

// The Xenos register file, as programmed through the command stream. Only the
// registers the protocol needs are ever read back.
std::array<uint32_t, 0x8000> g_gpuRegisters{};

constexpr uint32_t kRegScratchUmsk = 0x1DC;
constexpr uint32_t kRegScratchAddr = 0x1DD;
constexpr uint32_t kRegScratchReg0 = 0x578;

constexpr uint32_t kOpNop = 0x10;
constexpr uint32_t kOpWaitRegMem = 0x3C;
constexpr uint32_t kOpMemWrite = 0x3D;
constexpr uint32_t kOpIndirectBuffer = 0x3F;
constexpr uint32_t kOpIndirectBufferPfd = 0x37;
constexpr uint32_t kOpInterrupt = 0x54;
constexpr uint32_t kOpEventWriteShd = 0x58;

// The runtime's own swap packet. D3D reserves 64 dwords in the command buffer
// and passes their address to VdSwap; the KERNEL is what fills them with the
// swap commands (leaving them unwritten desyncs any parser: the stale bytes
// there are not packets, and the frame's fences behind them are skipped --
// measured as the transient scene-phase "GPU is hung" episodes). The encoding
// of the fill is private between the kernel and its GPU, so this pair uses an
// opcode Xenos does not define and sizes it to the reservation.
constexpr uint32_t kOpRuntimeSwap = 0x7F;
constexpr uint32_t kSwapReservationDwords = 64;

// Addresses in packets carry the Xenos endian mode in their low two bits.
// Mode 2 (8-in-32) is a full byte swap, which for guest big-endian memory is
// the plain guest store; the observed stream uses mode 2 everywhere. Mode 0
// stores untranslated.
void StoreEndian(uint32_t addressWord, uint32_t value)
{
    const uint32_t address = addressWord & ~3u;
    const uint32_t endian = addressWord & 3u;
    if (endian == 0)
        *gears::Memory().Translate<uint32_t>(address) = value;
    else if (endian == 2)
        StoreGuest32(address, value);
    else
    {
        lucent::warn("gpu", "unhandled endian mode {} storing to {:#x}", endian, address);
        StoreGuest32(address, value);
    }
}

uint32_t LoadEndian(uint32_t addressWord)
{
    const uint32_t address = addressWord & ~3u;
    const uint32_t endian = addressWord & 3u;
    if (endian == 0)
        return *gears::Memory().Translate<uint32_t>(address);
    return ReadGuest32(address);
}

void WriteGpuRegister(uint32_t reg, uint32_t value)
{
    reg &= 0x7FFF;
    g_gpuRegisters[reg] = value;

    // Scratch write-back: the title programs SCRATCH_ADDR/SCRATCH_UMSK (seen
    // both in the system command buffer and in the stream) and then writes
    // SCRATCH_REGs from the stream to publish values to the CPU -- among them
    // the ISR pending mask (REG0), and the completion callback and its
    // argument (REG4/REG5), which the ISR at 0x82221C60 reads from
    // SCRATCH_ADDR+0x10/+0x14.
    if (reg >= kRegScratchReg0 && reg < kRegScratchReg0 + 8)
    {
        const uint32_t n = reg - kRegScratchReg0;
        if (g_gpuRegisters[kRegScratchUmsk] & (1u << n))
        {
            const uint32_t address = g_gpuRegisters[kRegScratchAddr] + n * 4;
            StoreGuest32(address, value);
            lucent::debug("gpu", "scratch write-back reg{} = {:#x} -> {:#x}", n, value, address);
        }
    }
}

struct CommandProcessor
{
    InterruptThreadState interruptState;

    // Where the packet being executed came from (buffer base, or 0 for the
    // ring, and the word index of its header) -- provenance for diagnostics.
    uint32_t sourceBase = 0;
    uint32_t sourceIndex = 0;

    void HandleType3(uint32_t opcode, const uint32_t* data, uint32_t count, int depth)
    {
        switch (opcode)
        {
        case kOpIndirectBuffer:
        case kOpIndirectBufferPfd:
            if (count >= 2)
            {
                const uint32_t address = data[0] & ~3u;
                const uint32_t words = data[1] & 0xFFFFF;
                if (depth < 8 && address != 0 && words != 0)
                    ExecuteLinear(address, words, depth + 1);
            }
            break;

        case kOpWaitRegMem:
            if (count >= 5)
                WaitRegMem(data);
            break;

        case kOpMemWrite:
            for (uint32_t i = 1; i < count; i++)
                StoreEndian(data[0] + (i - 1) * 4, data[i]);
            break;

        case kOpEventWriteShd:
            // data: [initiator, address|endian, value]. The event pipeline on
            // hardware defers the write until the work retires; with no work
            // executed there is nothing to defer past, so it completes now.
            if (count >= 3)
            {
                StoreEndian(data[1], data[2]);
                lucent::debug("gpu", "EVENT_WRITE_SHD {:#x} <- {:#x} (from {:#x}[{:#x}])",
                    data[1] & ~3u, data[2], sourceBase, sourceIndex);
            }
            break;

        case kOpRuntimeSwap:
            // Frame boundary written by VdSwap. data[0] is the front buffer
            // address recorded there; nothing is presented, but the packet's
            // execution is what proves the stream stayed parseable through
            // the swap block.
            lucent::debug("gpu", "swap packet: front buffer {:#x}", count >= 1 ? data[0] : 0u);
            break;

        case kOpInterrupt:
            // data: [cpu mask]. Raises the graphics interrupt with source 1
            // (command completion) on each CPU in the mask.
            if (count >= 1)
            {
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                {
                    if (data[0] & (1u << cpu))
                    {
                        lucent::debug("gpu", "INTERRUPT -> cpu {}", cpu);
                        interruptState.Dispatch(1, cpu);
                    }
                }
            }
            break;

        default:
            break; // no draw hardware behind this; skipped by count
        }
    }

    void WaitRegMem(const uint32_t* data)
    {
        const uint32_t waitInfo = data[0];
        const uint32_t poll = data[1];
        const uint32_t ref = data[2];
        const uint32_t mask = data[3];

        const auto start = std::chrono::steady_clock::now();
        bool reported = false;
        for (;;)
        {
            const uint32_t raw = (waitInfo & 0x10)
                ? LoadEndian(poll)
                : g_gpuRegisters[poll & 0x7FFF];
            const uint32_t value = raw & mask;

            bool matched;
            switch (waitInfo & 7)
            {
            case 1: matched = value < ref; break;
            case 2: matched = value <= ref; break;
            case 3: matched = value == ref; break;
            case 4: matched = value != ref; break;
            case 5: matched = value >= ref; break;
            case 6: matched = value > ref; break;
            default: matched = true; break;
            }
            if (matched)
                return;

            std::this_thread::sleep_for(std::chrono::microseconds(50));
            if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
            {
                reported = true;
                lucent::error("gpu", "WAIT_REG_MEM stuck: info={:#x} poll={:#x} ref={:#x}"
                    " mask={:#x} value={:#x}", waitInfo, poll, ref, mask, value);
            }
        }
    }

    // Executes `words` dwords of packets at a linear guest physical address
    // (an indirect buffer).
    void ExecuteLinear(uint32_t base, uint32_t words, int depth)
    {
        uint32_t i = 0;
        while (i < words)
        {
            const uint32_t header = ReadGuest32(base + i * 4);
            sourceBase = base;
            sourceIndex = i;
            ++i;
            i += ExecutePacket(header, [&](uint32_t w) {
                return ReadGuest32(base + (i + w) * 4);
            }, words - i, depth);
        }
    }

    // Executes one packet whose header has been consumed; `fetch(w)` reads
    // data word w. Returns the number of data words consumed.
    template <typename Fetch>
    uint32_t ExecutePacket(uint32_t header, Fetch&& fetch, uint32_t available, int depth)
    {
        const uint32_t type = header >> 30;
        if (header == 0 || type == 2)
            return 0;

        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        const uint32_t usable = std::min(count, available);

        if (type == 0)
        {
            const uint32_t baseRegister = header & 0x7FFF;
            const bool oneRegister = (header & 0x8000) != 0;
            for (uint32_t w = 0; w < usable; w++)
                WriteGpuRegister(oneRegister ? baseRegister : baseRegister + w, fetch(w));
            return count;
        }
        if (type == 1)
        {
            if (usable >= 1) WriteGpuRegister(header & 0x7FF, fetch(0));
            if (usable >= 2) WriteGpuRegister((header >> 11) & 0x7FF, fetch(1));
            return count;
        }

        // TYPE3. The handled opcodes carry at most 18 words; larger packets
        // are state uploads, skipped without copying.
        const uint32_t opcode = (header >> 8) & 0x7F;
        uint32_t data[20];
        const uint32_t copy = std::min<uint32_t>(usable, 20);
        for (uint32_t w = 0; w < copy; w++)
            data[w] = fetch(w);
        HandleType3(opcode, data, copy, depth);
        return count;
    }

    // The ring consumer. rptr/wptr are dword indices; the write pointer is
    // whatever the title last stored to CP_RB_WPTR through the device window.
    void Run()
    {
        if (!interruptState.Init())
        {
            lucent::error("gpu", "cannot create the command processor's guest block");
            return;
        }

        lucent::info("gpu", "command processor consuming ring {:#x} ({} bytes)",
            g_ringBuffer.base, g_ringBuffer.Bytes());

        constexpr uint32_t kCpRbWptr = 0x7FC80714;
        const uint32_t dwords = g_ringBuffer.Dwords();
        uint32_t rptr = 0;
        StoreGuest32(g_ringBuffer.readPtrWriteBackAddress, rptr);

        for (;;)
        {
            const uint32_t wptr = ReadGuest32(kCpRbWptr) & (dwords - 1);
            if (wptr == rptr)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                continue;
            }

            const uint32_t header = ReadGuest32(g_ringBuffer.base + rptr * 4);
            sourceBase = 0;
            sourceIndex = rptr;
            rptr = (rptr + 1) & (dwords - 1);
            const uint32_t consumed = ExecutePacket(header, [&](uint32_t w) {
                return ReadGuest32(g_ringBuffer.base + ((rptr + w) & (dwords - 1)) * 4);
            }, dwords, 0);
            rptr = (rptr + consumed) & (dwords - 1);

            StoreGuest32(g_ringBuffer.readPtrWriteBackAddress, rptr);
        }
    }
};

void CommandProcessorThread()
{
    CommandProcessor cp;
    cp.Run();
}

std::atomic<bool> g_commandProcessorStarted{false};

void StartCommandProcessor()
{
    if (g_ringBuffer.base == 0 || g_ringBuffer.readPtrWriteBackAddress == 0)
        return;
    if (g_commandProcessorStarted.exchange(true))
        return;
    std::thread(CommandProcessorThread).detach();
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
// each new block is discovered: no devices are modelled behind it, so a write
// just lands in the memory. That is also how submission works: the title's
// store of the ring write pointer to CP_RB_WPTR (0x7FC80714) is read back from
// this window by the command processor thread.
bool CommitDeviceWindow(GuestMemory& memory)
{
    constexpr uint32_t kDeviceWindowBase = 0x7FC00000;
    constexpr uint32_t kDeviceWindowSize = 0x00400000;

    if (!memory.Commit(kDeviceWindowBase, kDeviceWindowSize))
        return false;

    // One register in this window is not inert, because leaving it zero is not
    // a neutral choice.
    //
    // 0x7FC86544 bit 0 gates the vblank path of the title's graphics interrupt
    // handler: the handler runs `if (source == 0 && (reg & 1))`. The whole
    // image reads that address exactly once -- at 0x82221CFC, inside that
    // handler -- and never writes it, so it is a status bit the GPU owns and
    // the title only samples.
    //
    // The runtime is the GPU here and it does deliver vblank, at 60 Hz from a
    // host thread. Reporting the bit clear while delivering the interrupt
    // describes a machine that cannot exist. It stays set rather than
    // being latched and cleared because the title never acknowledges it.
    //
    // The read at 0x82221CFC is a plain `lwz` -- a big-endian load, not
    // `lwbrx` -- so the word is stored guest-byte-order. (Storing it
    // little-endian left the ISR reading 0x01000000, bit 0 clear, and the
    // vblank path never executed.)
    constexpr uint32_t kVblankStatusRegister = 0x7FC86544;
    *memory.Translate<uint32_t>(kVblankStatusRegister) = ByteSwap(1u);

    // The frame-pacing callback the stream installs (0x8223E648) computes how
    // far the raster is through the frame as
    //     *(0x7FC86530) * 100 / (*(0x7FC86584) & 0xFFF)
    // i.e. current scanline over vertical total. A zero vertical total is a
    // display that cannot exist (and divides by zero); the vertical total for
    // the 1280x720p60 mode this runtime reports is 750 lines (CEA-861). The
    // scanline register stays 0: this display is always in vblank, which is
    // consistent with the vblank status bit above. Both are read with plain
    // `lwz`, so guest byte order.
    constexpr uint32_t kVerticalTotalRegister = 0x7FC86584;
    *memory.Translate<uint32_t>(kVerticalTotalRegister) = ByteSwap(750u);

    lucent::info("gpu", "device MMIO window {:#x}..{:#x} committed; vblank status set",
        kDeviceWindowBase, kDeviceWindowBase + kDeviceWindowSize);
    return true;
}

} // namespace gears

void __imp__VdInitializeEngines(PPCContext& __restrict ctx, uint8_t*)
{
    if (const char* watch = getenv("GEARS_PM4_WATCH"))
    {
        g_pm4WatchAddress = uint32_t(strtoul(watch, nullptr, 16));
        lucent::info("gpu", "command stream will be traced for writes to {:#x}",
            g_pm4WatchAddress);
    }

    lucent::warn("gpu", "VdInitializeEngines -- protocol-only GPU: no draws will be executed");
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
    lucent::info("gpu", "ring buffer at {:#x}, {} bytes", g_ringBuffer.base, g_ringBuffer.Bytes());
    StartCommandProcessor();
    ctx.r3.u64 = 0;
}

void __imp__VdEnableRingBufferRPtrWriteBack(PPCContext& __restrict ctx, uint8_t*)
{
    g_ringBuffer.readPtrWriteBackAddress = ctx.r3.u32;
    g_ringBuffer.readPtrWriteBackBlockSize = ctx.r4.u32;
    lucent::info("gpu", "ring buffer read-pointer write-back at {:#x}",
        g_ringBuffer.readPtrWriteBackAddress);
    StartCommandProcessor();
    ctx.r3.u64 = 0;
}

void __imp__VdSetSystemCommandBufferGpuIdentifierAddress(PPCContext& __restrict ctx, uint8_t*)
{
    g_systemCommandBufferGpuIdentifier = ctx.r3.u32;
    lucent::info("gpu", "system command buffer GPU identifier at {:#x}", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

namespace
{
// The console raises this from the GPU at vblank, and titles advance real
// state machines from it. Command-completion interrupts (source 1) come from
// the command processor thread when the stream executes an INTERRUPT packet;
// this thread only provides the 60 Hz vblank (source 0).
//
// Set GEARS_NO_VBLANK=1 to disable it, which is useful for telling failure
// modes apart.
void VblankThread()
{
    InterruptThreadState state;
    if (!state.Init())
    {
        lucent::error("gpu", "cannot create the vblank thread's guest block");
        return;
    }

    lucent::info("gpu", "vblank thread driving interrupt callback {:#x} at 60 Hz",
        g_graphicsInterruptCallback);

    uint32_t tick = 0;
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(16667));

        // Sampled from here rather than from VdSwap: the title submits one
        // frame and then waits, so by the time it is stuck there are no more
        // swaps to hang a trace off, and the ring only has contents to read
        // after that first submission.
        if (g_pm4WatchAddress != 0 && g_ringBuffer.base != 0 && ++tick % 60 == 0)
        {
            gears::TraceCommandStream(g_ringBuffer.base,
                g_ringBuffer.Dwords(), g_pm4WatchAddress);
        }

        state.Dispatch(0, 0);
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

// VdSwap(swapBlock, ..., frontBufferPtrPtr, ...): D3D's Present reserves 64
// dwords in the command buffer and hands their address over as r3; the kernel
// writes the swap command sequence into them, and the stream then flows on to
// the frame's fence packets. Leaving the block unwritten is not a neutral
// omission: whatever stale bytes sit there desync the command processor and
// the frame's fences are skipped (measured: transient scene-phase "GPU is
// hung" episodes, ~5 s each, until a later frame's fence rescued the ticket
// lock). The fill is one runtime-private packet spanning the whole
// reservation, carrying the front buffer address for when presentation
// becomes real.
void __imp__VdSwap(PPCContext& __restrict ctx, uint8_t*)
{
    const uint64_t frame = g_frameCount.fetch_add(1) + 1;
    if (frame == 1 || frame % 60 == 0)
        lucent::info("gpu", "VdSwap: {} frames submitted (nothing presented)", frame);

    const uint32_t block = ctx.r3.u32;
    if (block != 0)
    {
        const uint32_t frontBuffer =
            ctx.r8.u32 != 0 ? ReadGuest32(ctx.r8.u32) : 0;

        StoreGuest32(block, (3u << 30) | ((kSwapReservationDwords - 2) << 16)
            | (kOpRuntimeSwap << 8));
        StoreGuest32(block + 4, frontBuffer);
        for (uint32_t i = 2; i < kSwapReservationDwords; i++)
            StoreGuest32(block + i * 4, 0);

        lucent::debug("gpu", "VdSwap: swap packet at {:#x}, front buffer {:#x}",
            block, frontBuffer);
    }

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
// It is handed out as real, committed memory; the packets the title writes
// into it (scratch write-back setup among them) are executed when the stream
// points an indirect buffer at it.
void __imp__VdGetSystemCommandBuffer(PPCContext& __restrict ctx, uint8_t*)
{
    static uint32_t s_commandBuffer = 0;
    if (s_commandBuffer == 0)
    {
        uint32_t size = 0x10000;
        s_commandBuffer = gears::PhysicalHeap().Allocate(
            0, size, gears::kMemCommit | gears::kMemLargePages);
        lucent::info("gpu", "system command buffer at {:#x}", s_commandBuffer);
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
