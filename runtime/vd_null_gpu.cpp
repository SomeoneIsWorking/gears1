// The Xenos driver surface and PM4 command processor. It owns the guest ring,
// register stream, fences, scratch write-backs and graphics interrupts, and
// captures ordered draw/state snapshots. Rasterization and presentation remain
// separate host owners in gpu_draw.cpp and gpu_present.cpp.
#include "import_stub.h"
#include "guest_clock.h"

#include <algorithm>
#include <array>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <mutex>
#include <thread>

#include <byteswap.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_heap.h"
#include "guest_write_watch.h"
#include "gpu_present.h"
#include "gpu_packet_memory.h"
#include "gpu_register_watch.h"
#include "input.h"
#include "debug_http.h"
#include "graphics_probe.h"
#include "graphics_probe_render.h"
#include "frame_probe_capture.h"
#include "gpu_draw.h"
#include "gpu_endian.h"
#include "render_thread.h"
#include "rhi_semantic_stream.h"
#include "frame_capture.h"
#include "frame_content.h"
#include "hle_d3d.h"
#include "guest_thread.h"
#include "wait_probe.h"
#include "fault_report.h"
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
    uint32_t lastInnerCallback = 0xFFFFFFFF;

    bool Init()
    {
        if (!gears::CreateGuestThreadBlock(gears::Memory(), 0x10000, block))
            return false;
        ctx.r13.u32 = block.pcrAddress;
        ctx.fpscr.loadFromHost();
        gears::SetGuestThreadName("gpu-isr");
        ready = true;
        return true;
    }

    void Dispatch(uint32_t source, uint32_t cpu)
    {
        if (!ready || g_graphicsInterruptCallback == 0)
            return;
        std::lock_guard<std::mutex> lock(g_interruptMutex);
        uint8_t *base = gears::Memory().Base();
        *gears::Memory().Translate<uint8_t>(block.pcrAddress + kPcrCpuNumber) = uint8_t(cpu);
        ctx.r1.u32 = block.stackBase - 0x100;
        ctx.r3.u32 = source;
        ctx.r4.u32 = g_graphicsInterruptContext;
        // The command-completion arm invokes the callback stored in the
        // graphics context to signal the draw worker. A null slot silently
        // loses that signal, so report each distinct value once.
        if (source == 1)
        {
            const uint32_t inner =
                ReadGuest32(ReadGuest32(g_graphicsInterruptContext + 0x2A14) + 0x10);
            if (inner != lastInnerCallback)
            {
                lastInnerCallback = inner;
                // The observed event array is per CPU within the graphics
                // worker pool; include its selected slot in the diagnostic.
                const uint32_t pool = ReadGuest32(ReadGuest32(0x82000868));
                lucent::debug(
                    "gpu", "graphics ISR inner callback -> {:#x} (pool {:#x}, cpu{} event {:#x})",
                    inner, pool, cpu, pool + 0x2BDC + cpu * 0x38);
            }
        }
        (PPC_LOOKUP_FUNC(base, g_graphicsInterruptCallback))(ctx, base);
    }
};

// ---------------------------------------------------------------------------
// PM4 execution.

// The Xenos register file, as programmed through the command stream. Only the
// registers the protocol needs are ever read back.
std::array<uint32_t, 0x8000> g_gpuRegisters{};

// The shared register snapshot handed to draws, and whether anything has
// changed since it was taken. Consecutive draws overwhelmingly share identical
// register state, so a per-draw copy of 128 KB was pure waste.
std::shared_ptr<const std::vector<uint32_t>> g_registerSnapshot;
bool g_registersDirty = true;
uint64_t g_registerSnapshots = 0;

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
constexpr uint32_t kOpEventWriteExt = 0x5A;
constexpr uint32_t kOpEventWriteZpd = 0x5B;

// The record EVENT_WRITE_ZPD writes, exactly as Xenia's
// xe_gpu_depth_sample_counts declares it: eight little-endian counters, A and B
// summed by D3D, and the query result is END minus BEGIN. Named here rather
// than written as raw offsets because the ONE field that matters (ZPass) sits
// at +16, and a hand-counted offset into an eight-field record is how a
// diagnostic ends up reporting a different counter than it claims to.
struct GuestDepthSampleCounts
{
    uint32_t total_A, total_B;
    uint32_t zfailA, zfailB;
    uint32_t zpassA, zpassB;
    uint32_t stencilFailA, stencilFailB;
};
static_assert(sizeof(GuestDepthSampleCounts) == 0x20,
              "the ZPD record is 0x20 bytes; the write below memsets that much");
static_assert(offsetof(GuestDepthSampleCounts, zpassA) == 16,
              "D3D reads the occlusion result from ZPass, at +16 in this record");

// How much the reported ZPass counter grows per query under
// GEARS_GPU_ZPD_VISIBLE. Only the SIGN of END-BEGIN matters to a visibility
// test, but a title that scales an effect by the sample count wants a number
// with a plausible magnitude, so this is roughly a 720p screenful.
constexpr uint32_t kZpdSamplesPerQuery = 1280 * 720;

// RB_SAMPLE_COUNT_ADDR: where EVENT_WRITE_ZPD writes its sample-count record.
constexpr uint32_t kRegSampleCountAddr = 0x2325;

// ---------------------------------------------------------------------------
// The display gamma ramp (DC_LUT_*).
//
// The scan-out hardware puts every presented pixel through a per-channel lookup
// table. Nothing in this runtime did, so the frame reached the screen with
// whatever curve the composite produced -- which is one measured difference
// against the reference renderer (catalog #78).
//
// The ramp cannot be recovered from a register snapshot: it is uploaded ENTRY BY
// ENTRY, each write landing in the same register, so only the last one survives
// in the register file. It has to be accumulated as the writes go past, which is
// what this does.
//
// Register indices and write semantics mirror extern/xenia
// src/xenia/gpu/command_processor.cc (WriteRegister's DC_LUT cases) and
// registers.h (DC_LUT_30_COLOR is blue:10, green:10, red:10).
constexpr uint32_t kRegDcLutRwMode = 0x1921;
constexpr uint32_t kRegDcLutRwIndex = 0x1922;
constexpr uint32_t kRegDcLutSeqColor = 0x1923;
constexpr uint32_t kRegDcLutPwlData = 0x1924;
constexpr uint32_t kRegDcLut30Color = 0x1925;
constexpr uint32_t kRegDcLutWriteEnMask = 0x1927;
constexpr uint32_t kRegDcLutaControl = 0x1930;

// VGT_DRAW_INITIATOR (Xenia registers.h / register_table.inc). The DRAW_INDX /
// DRAW_INDX_2 packet carries this value in its payload; the sequencer latches it
// into the register file on hardware. Mirroring it makes prim_type / index_size
// live for the system-constants derivation instead of reading stale-zero.
constexpr uint32_t kRegDrawInitiator = 0x21FC;

// The runtime's own swap packet. D3D reserves 64 dwords in the command buffer
// and passes their address to VdSwap; the KERNEL is what fills them with the
// swap commands (leaving them unwritten desyncs any parser: the stale bytes
// there are not packets, and the frame's fences behind them are skipped --
// measured as the transient scene-phase "GPU is hung" episodes). The encoding
// of the fill is private between the kernel and its GPU, so this pair uses an
// opcode Xenos does not define and sizes it to the reservation.
constexpr uint32_t kOpRuntimeSwap = 0x7F;
constexpr uint32_t kSwapReservationDwords = 64;

// Sequencer instruction-memory loads: this is where a shader actually becomes
// the bound shader, at the hardware level. IM_LOAD points at microcode in
// physical memory; IM_LOAD_IMMEDIATE carries the microcode inline in the
// packet. Contract mirrored from extern/xenia
// src/xenia/gpu/pm4_command_processor_implement.h
// (ExecutePacketType3_IM_LOAD / _IM_LOAD_IMMEDIATE).
constexpr uint32_t kOpImLoad = 0x27;
constexpr uint32_t kOpImLoadImmediate = 0x2B;

// Constant-file loads. The sequencer keeps four constant files plus the general
// register block, all addressed inside the same register space the rest of the
// stream writes. SET_CONSTANT loads them from the ring by (index,type);
// LOAD_ALU_CONSTANT loads a range from physical memory; SET_CONSTANT2 /
// SET_SHADER_CONSTANTS write a raw register index directly. Opcodes and
// semantics mirror extern/xenia src/xenia/gpu/xenos.h (PM4_* enum) and
// pm4_command_processor_implement.h (ExecutePacketType3_SET_CONSTANT etc).
constexpr uint32_t kOpSetConstant = 0x2D;
constexpr uint32_t kOpLoadAluConstant = 0x2F;
constexpr uint32_t kOpSetConstant2 = 0x55;
constexpr uint32_t kOpSetShaderConstants = 0x56;

// Base register index of each constant file, from Xenia's WriteALURangeFromRing
// et al (src/xenia/gpu/command_processor.cc): the (index,type) pair in a
// SET_CONSTANT resolves to one of these plus index.
//   type 0 ALU float   -> 0x4000 (256 vec4 = 1024 dwords)
//   type 1 FETCH        -> 0x4800 (32 * 6 = 192 dwords; vertex fetch is 2/slot)
//   type 2 BOOL         -> 0x4900
//   type 3 LOOP         -> 0x4908
//   type 4 REGISTERS    -> 0x2000 (general register block)
constexpr uint32_t kConstBaseAlu = 0x4000;
constexpr uint32_t kConstBaseFetch = 0x4800;
constexpr uint32_t kConstBaseBool = 0x4900;
constexpr uint32_t kConstBaseLoop = 0x4908;
constexpr uint32_t kConstBaseRegisters = 0x2000;

// ---------------------------------------------------------------------------
// Bound-shader capture.
//
// The offline corpus (tools/shader_extract.py) is everything that sits
// uncompressed in the cooked packages. It says nothing about which shaders the
// running title binds. The sequencer load packets do: whatever microcode the
// GPU is handed here is, by definition, what the title bound. Capturing at this
// point needs no knowledge of the D3D shader-set API and covers every path,
// including the movie player's hand-built command buffer.
//
// Enabled with GEARS_SHADER_CAPTURE=1; containers go to
// GEARS_SHADER_CAPTURE_DIR (default scratch/shaders/bound).
// ---------------------------------------------------------------------------
struct BoundShader
{
    uint32_t type = 0; // xenos::ShaderType: 0 vertex, 1 pixel
    uint32_t dwords = 0;
    uint32_t address = 0; // physical address (IM_LOAD) or 0 (immediate)
    uint64_t loads = 0;
    bool immediate = false;
    // The microcode as big-endian bytes (as the GPU reads it, and as the Xenos
    // translator consumes it). Kept so the guest-draw backend can translate the
    // bound pair at draw time without re-reading it from a capture file.
    std::vector<uint8_t> ucode;
};

struct ShaderCaptureState
{
    bool enabled = false;    // keep the microcode in memory (always: the renderer needs it)
    bool writeFiles = false; // also write it to cap.dir (GEARS_SHADER_CAPTURE)
    bool ready = false;
    std::string dir = "scratch/shaders/bound";
    std::map<uint64_t, BoundShader> shaders; // ucode hash -> record
    uint64_t imLoads = 0;
    uint64_t imLoadsImmediate = 0;
    uint64_t truncated = 0;        // packet claimed more ucode than the buffer held
    uint64_t rejected = 0;         // implausible size
    uint64_t activeVertexHash = 0; // last vertex ucode bound (for the const dump)
    uint64_t activePixelHash = 0;  // last pixel ucode bound
} g_shaderCapture;

uint64_t Fnv1a64(const uint8_t *p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// `ucode` holds the microcode as big-endian bytes, exactly as the GPU reads it,
// which is also what tools/xenos_translate consumes (std::endian::big).
void RecordBoundShader(uint32_t type, uint32_t address, bool immediate,
                       const std::vector<uint8_t> &ucode)
{
    auto &cap = g_shaderCapture;
    const uint64_t hash = Fnv1a64(ucode.data(), ucode.size());
    (type == 0 ? cap.activeVertexHash : cap.activePixelHash) = hash;
    auto it = cap.shaders.find(hash);
    if (it != cap.shaders.end())
    {
        ++it->second.loads;
        return;
    }
    BoundShader s;
    s.type = type;
    s.dwords = uint32_t(ucode.size() / 4);
    s.address = address;
    s.immediate = immediate;
    s.loads = 1;
    s.ucode = ucode;
    cap.shaders.emplace(hash, s);

    if (!cap.writeFiles)
        return;
    const std::string path =
        std::format("{}/{}_{:016x}.ucode", cap.dir, type == 0 ? "vs" : "ps", hash);
    if (FILE *f = std::fopen(path.c_str(), "wb"))
    {
        std::fwrite(ucode.data(), 1, ucode.size(), f);
        std::fclose(f);
    }
    else
    {
        lucent::warn("gpu", "shader capture: cannot write {}", path);
    }
}

void ShaderCaptureManifest()
{
    auto &cap = g_shaderCapture;
    if (!cap.writeFiles)
        return;
    const std::string path = cap.dir + "/manifest.csv";
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f)
        return;
    std::fprintf(f, "file,type,ucode_dwords,address,immediate,loads\n");
    for (const auto &[hash, s] : cap.shaders)
        std::fprintf(f, "%s_%016llx.ucode,%s,%u,0x%08X,%d,%llu\n", s.type == 0 ? "vs" : "ps",
                     (unsigned long long)hash, s.type == 0 ? "vs" : "ps", s.dwords, s.address,
                     s.immediate ? 1 : 0, (unsigned long long)s.loads);
    std::fclose(f);
}

void ShaderCaptureInit()
{
    auto &cap = g_shaderCapture;
    if (cap.ready)
        return;
    cap.ready = true;
    // Capturing the microcode is not optional: the renderer translates the pair
    // bound at each draw, and this is how that microcode is kept in memory. What
    // GEARS_SHADER_CAPTURE controls is whether a COPY is also written to disk for
    // the offline tools (tools/xenos_translate, tools/compare_bound_shaders.py).
    cap.enabled = true;
    cap.writeFiles = lucent::config::flag("SHADER_CAPTURE");
    if (!cap.writeFiles)
        return;
    const std::string &dir = lucent::config::text("SHADER_CAPTURE_DIR");
    if (!dir.empty())
        cap.dir = dir;
    if (std::system(("mkdir -p '" + cap.dir + "'").c_str()) != 0)
        lucent::warn("gpu", "shader capture: cannot create {}", cap.dir);
    lucent::info("gpu", "shader capture armed (PM4 IM_LOAD), writing microcode to {}", cap.dir);
}

// The accumulated ramp, and how it was uploaded. Both matter: an entry table
// written through DC_LUT_30_COLOR and one written through DC_LUT_SEQ_COLOR are
// the same ramp, but the reference renderer only implements the SEQ_COLOR path,
// so knowing which this title uses is the difference between "we now match the
// hardware" and "we now differ from the oracle on purpose".
struct GammaRamp
{
    // 256 entries, each packed as the hardware's DC_LUT_30_COLOR:
    // blue in bits 0..9, green in 10..19, red in 20..29.
    uint32_t table[256] = {};
    uint32_t seqWrites = 0;      // uploaded a component at a time
    uint32_t directWrites = 0;   // uploaded a whole entry at a time
    uint32_t pwlWrites = 0;      // the piecewise-linear form instead
    uint32_t rwComponent = 0;    // which channel a sequential write targets
    bool reported = false;       // the "none by the first frame" line
    uint32_t reportedWrites = 0; // what the last summary covered

    // Linear, i.e. what the hardware does with no ramp programmed: entry i maps
    // to i scaled from 8 to 10 bits. Any pixel put through this comes out
    // unchanged, so a ramp that was never uploaded cannot alter the image --
    // which is what makes "we apply no ramp" and "we apply this default" the
    // same picture, and why the counters above are needed to tell them apart.
    GammaRamp()
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            const uint32_t v = i * 0x3FF / 0xFF;
            table[i] = v | (v << 10) | (v << 20);
        }
    }
};
GammaRamp g_gammaRamp;

// How many draws this frame had already been recorded when a watched register
// was written. "Before draw N" is the attribution a shared constant needs.
uint32_t g_regWatchDrawOrdinal = 0;

void WriteGpuRegister(uint32_t reg, uint32_t value)
{
    reg &= 0x7FFF;
    // SCRATCH_ADDR/SCRATCH_UMSK retarget every subsequent write-back, so a
    // stray write to either silently redirects the ISR's callback slot and the
    // stream's completion flags. Report every change to them.
    if ((reg == kRegScratchAddr || reg == kRegScratchUmsk) && g_gpuRegisters[reg] != value)
        lucent::debug("gpu", "SCRATCH_{} {:#x} -> {:#x}", reg == kRegScratchAddr ? "ADDR" : "UMSK",
                      g_gpuRegisters[reg], value);
    if (g_gpuRegisters[reg] != value)
        g_registersDirty = true;
    gears::ObserveGpuRegisterWrite(reg, value, g_gpuRegisters[reg], g_regWatchDrawOrdinal);
    g_gpuRegisters[reg] = value;

    // Gamma ramp uploads. The write enable mask is BLUE, GREEN, RED (bit 2 is
    // red), which is the reverse of the order the data arrives in, and bits 0:5
    // of a sequential value are hardwired to zero -- both per Xenia's
    // WriteRegister, which is the contract this mirrors.
    switch (reg)
    {
    case kRegDcLutRwIndex:
        // A new index restarts the channel sequence.
        g_gammaRamp.rwComponent = 0;
        break;

    case kRegDcLut30Color:
    {
        // A whole entry at once, as opposed to the component-at-a-time
        // SEQ_COLOR form. This title uploads its entire ramp this way: measured,
        // 256 whole-entry writes and zero sequential ones.
        //
        // The reference renderer implements the same path with the same
        // auto-increment (extern/xenia command_processor.cc, WriteRegister's
        // DC_LUT_30_COLOR case), which is a useful independent check on the
        // semantics below -- they were worked out here from the register file's
        // end state before that case was found.
        const uint32_t index = g_gpuRegisters[kRegDcLutRwIndex] & 0xFF;
        const uint32_t mask = g_gpuRegisters[kRegDcLutWriteEnMask];
        uint32_t entry = g_gammaRamp.table[index];
        if (mask & 4)
            entry = (entry & ~(0x3FFu << 20)) | (((value >> 20) & 0x3FF) << 20);
        if (mask & 2)
            entry = (entry & ~(0x3FFu << 10)) | (((value >> 10) & 0x3FF) << 10);
        if (mask & 1)
            entry = (entry & ~0x3FFu) | (value & 0x3FF);
        g_gammaRamp.table[index] = entry;
        ++g_gammaRamp.directWrites;
        // The index AUTO-INCREMENTS, as it does for the sequential form after a
        // full triple. Without this every write of an upload lands in the same
        // entry: the first attempt here left 255 entries at their default and
        // entry 0 holding the last value written, which reads as "the title's
        // ramp is linear apart from one entry" -- a conclusion about the title
        // that was really a defect in this code.
        //
        // It is also what makes the register file's end state make sense: after
        // 256 writes an 8-bit index wraps back to 0, which is exactly what a
        // capture taken later shows (DC_LUT_RW_INDEX = 0).
        g_gpuRegisters[kRegDcLutRwIndex] =
            (g_gpuRegisters[kRegDcLutRwIndex] & ~0xFFu) | ((index + 1) & 0xFF);
        break;
    }

    case kRegDcLutSeqColor:
    {
        // One channel at a time, in red, green, blue order, advancing the index
        // after the third.
        const uint32_t index = g_gpuRegisters[kRegDcLutRwIndex] & 0xFF;
        const uint32_t mask = g_gpuRegisters[kRegDcLutWriteEnMask];
        const uint32_t component = g_gammaRamp.rwComponent;
        if (mask & (1u << (2 - component)))
        {
            const uint32_t v = (value >> 6) & 0x3FF; // bits 0:5 hardwired zero
            const uint32_t shift = component == 0 ? 20 : component == 1 ? 10 : 0;
            g_gammaRamp.table[index] =
                (g_gammaRamp.table[index] & ~(0x3FFu << shift)) | (v << shift);
            ++g_gammaRamp.seqWrites;
        }
        if (++g_gammaRamp.rwComponent >= 3)
        {
            g_gammaRamp.rwComponent = 0;
            g_gpuRegisters[kRegDcLutRwIndex] =
                (g_gpuRegisters[kRegDcLutRwIndex] & ~0xFFu) | ((index + 1) & 0xFF);
        }
        break;
    }

    case kRegDcLutPwlData:
        // The piecewise-linear form. COUNTED BUT NOT DECODED: this title has not
        // been seen to use it, and a half-implemented second path would be
        // indistinguishable from a working one until the day a title needs it.
        // If this counter is ever non-zero, that is the day.
        ++g_gammaRamp.pwlWrites;
        if (++g_gammaRamp.rwComponent >= 3)
            g_gammaRamp.rwComponent = 0;
        break;

    default:
        break;
    }

    // Scratch write-back: the title programs SCRATCH_ADDR/SCRATCH_UMSK (seen
    // both in the system command buffer and in the stream) and then writes
    // SCRATCH_REGs from the stream to publish values to the CPU -- among them
    // the ISR pending mask (REG0), and the completion callback and its
    // argument (REG4/REG5), which the interrupt path consumes from the matching
    // scratch slots.
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

// Names only for the opcodes this investigation reasons about; anything else
// prints as its number, which is enough to look up in Xenia's xenos.h.
std::string OpcodeName(uint32_t op)
{
    switch (op)
    {
    case 0x10:
        return "NOP";
    case 0x22:
        return "DRAW_INDX";
    case 0x23:
        return "VIZ_QUERY";
    case 0x25:
        return "SET_STATE";
    case 0x26:
        return "WAIT_FOR_IDLE";
    case 0x2D:
        return "SET_CONSTANT";
    case 0x2F:
        return "LOAD_ALU_CONSTANT";
    case 0x36:
        return "DRAW_INDX_2";
    case 0x37:
        return "IB_PFD";
    case 0x3B:
        return "INVALIDATE_STATE";
    case 0x3C:
        return "WAIT_REG_MEM";
    case 0x3D:
        return "MEM_WRITE";
    case 0x3F:
        return "IB";
    case 0x44:
        return "COND_EXEC";
    case 0x45:
        return "COND_WRITE";
    case 0x46:
        return "EVENT_WRITE";
    case 0x4B:
        return "SET_BIN_BASE_OFFSET";
    case 0x50:
        return "SET_BIN_MASK";
    case 0x51:
        return "SET_BIN_SELECT";
    case 0x54:
        return "INTERRUPT";
    case 0x55:
        return "SET_CONSTANT2";
    case 0x56:
        return "SET_SHADER_CONSTANTS";
    case 0x58:
        return "EVENT_WRITE_SHD";
    case 0x5A:
        return "EVENT_WRITE_EXT";
    case 0x5B:
        return "EVENT_WRITE_ZPD";
    case 0x60:
        return "SET_BIN_MASK_LO";
    case 0x61:
        return "SET_BIN_MASK_HI";
    case 0x62:
        return "SET_BIN_SELECT_LO";
    case 0x63:
        return "SET_BIN_SELECT_HI";
    case kOpRuntimeSwap:
        return "SWAP";
    default:
        return std::format("op{:#x}", op);
    }
}

struct CommandProcessor
{
    InterruptThreadState interruptState;

    // Where the packet being executed came from (buffer base, or 0 for the
    // ring, and the word index of its header) -- provenance for diagnostics.
    uint32_t sourceBase = 0;
    uint32_t sourceIndex = 0;

    // Highest VdSwap sequence executed; stale re-submitted copies are behind it.
    uint32_t lastSwapSequence = 0;

    // Where the CP's time goes between frame boundaries: total microseconds
    // spent inside WAIT_REG_MEM keyed by polled address/register, reported at
    // each executed swap packet. Diagnosis for the frame-rate investigation.
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> waitStats; // addr -> {count, us}

    // Per-frame packet census, for the "why is the same buffer submitted 44-88
    // times per frame" question. Xenos renders in EDRAM tiles and D3D's
    // predicated tiling REPLAYS the recorded command buffer once per tile,
    // bracketing each replay with SET_BIN_MASK/SET_BIN_SELECT; that would make
    // repeated IB submission entirely faithful. So the census keys opcodes by
    // depth (ring level vs inside an indirect buffer) and counts distinct IBs,
    // and the bin packets are counted whether or not they are acted on.
    // Unwrapped ring accounting. The masked difference (wptr - rptr) can never
    // report an overshoot -- once the read pointer passes the write pointer the
    // difference wraps and looks like an almost-full ring, which is exactly the
    // shape of a consumer that laps. So both pointers are also tracked
    // unwrapped, and the first packet whose consumption carries the read
    // pointer past everything written is reported.
    uint64_t rptrTotal = 0;
    uint64_t wptrTotal = 0;
    bool overshootReported = false;
    uint64_t frameWptrAdvance = 0;
    uint64_t frameRptrAdvance = 0;

    std::map<uint32_t, uint64_t> ringOpcodes;  // depth 0 TYPE3 opcode -> count
    std::map<uint32_t, uint64_t> innerOpcodes; // depth > 0 TYPE3 opcode -> count
    std::map<uint32_t, uint64_t> ibCounts;     // IB address -> submissions

    // Census of how the constant files are actually fed, to establish the path
    // rather than assume it: SET_CONSTANT by type, the memory/raw variants, and
    // the plain TYPE0 register writes that land in the ALU/fetch ranges (the
    // stream also programs these files directly, so both paths must be seen).
    uint64_t setConstantByType[8]{};     // SET_CONSTANT (0x2D) by type field
    uint64_t loadAluConstantByType[8]{}; // LOAD_ALU_CONSTANT (0x2F) by type field
    uint64_t setConstant2Packets = 0;
    uint64_t setShaderConstantsPackets = 0;
    uint64_t type0AluWrites = 0;   // TYPE0 dwords into 0x4000..0x47FF
    uint64_t type0FetchWrites = 0; // TYPE0 dwords into 0x4800..0x48FF
    bool constDumpDone = false;    // one-shot verification dump latch
    bool drawCaptureDone = false;  // one-shot hot-pair draw-param capture latch

    // Per-run draw census (GEARS_DRAW_CENSUS): every DRAW_INDX/_2 the CP
    // executes, keyed by (vs,ps) hash, so the real distribution of draws a run
    // reaches -- not just the hot pair -- is measured before generalising the
    // backend. Reported at the first swap and at shutdown.
    uint64_t drawsSeen = 0;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> drawPairs; // (vs,ps) -> count
    bool drawCensusReported = false;

    // Whole-frame guest-draw backend: accumulate every DRAW_INDX/_2 of the frame
    // with the register-file state live at that draw, then hand the whole ordered
    // list to gears::RenderFrame at the swap. This is the renderer -- it is not
    // gated, because a run that executes the guest's draws and shows none of them
    // is not a mode anyone wants. GEARS_DRAW_FRAME_AT/_COUNT bound WHICH frames
    // are rendered for a capture or a measurement; by default every frame is.
    std::vector<gears::FrameDrawItem> frameDraws;
    bool frameRenderDone = false;
    gears::FrameProbeCapture frameProbeCapture;
    // One frame capture per run (GEARS_DRAW_FRAME_DUMP), not one per frame.
    bool frameDumpWritten = false;
    // GEARS_DRAW_FRAME_DUMP_SKINNED: how many frames the character scan has
    // rejected, and the best it has seen. A run that scans for ten minutes and
    // finds nothing must say what it scanned -- silence would be
    // indistinguishable from a scan that never ran.
    uint32_t skinnedScans = 0;
    uint32_t skinnedBestIndices = 0;
    uint32_t skinnedBestDraws = 0;
    // GEARS_DRAW_FRAME_MIN_DRAWS: the content selector's state. Armed by the
    // first frame that reaches the threshold, opened on the frame after it, and
    // what the scan has SEEN so far -- a scan that finds nothing has to be able
    // to say what it looked at.
    bool contentArmed = false;
    bool contentGateOpen = false;
    uint32_t contentScans = 0;
    uint32_t contentBusiest = 0;
    uint32_t contentWait = 0;     // GEARS_DRAW_FRAME_AFTER_GAMEPLAY, counted down
    uint32_t contentCamHeld = 0;  // frames held by the camera gate
    double contentCamBest = 1e30; // closest any frame has come
    // GEARS_DRAW_FRAME_NEEDS: a CONTENT requirement on top of the draw count.
    // The frame this title reaches after N presents is not the same frame twice
    // running -- the shadow pass renders a different set of lights each time --
    // so a run aimed at a specific draw may simply not contain it, and a run
    // that does not contain it is SILENT about it rather than negative. This
    // holds the gate shut until a frame actually carries the draw in question.
    uint32_t contentHeld = 0;       // frames rejected for want of the draw
    uint32_t contentBestVerts = 0;  // biggest matching-shader draw seen so far
    uint32_t contentShaderSeen = 0; // frames that had the shader at all
    // EVERY DRAW THE GUEST ISSUED, AND EVERY REASON WE DROPPED ONE. Four paths
    // in CaptureFrameDraw discard a draw, and none of them was counted, so
    // "this frame had 800 draws" could never be distinguished from "this frame
    // had 2400 and we kept 800". Reported per frame with the denominator.
    uint32_t drawsOffered = 0;
    uint32_t drawsNoShaderPair = 0;
    uint32_t drawsZeroIndices = 0;
    uint32_t drawsImmediateIndex = 0;
    uint32_t drawsAfterFrameDone = 0;
    long framesRendered = 0;
    // Which GEARS_DRAW_FRAME_REPORT_EVERY bucket of the GUEST'S present counter
    // has already been reported. A bucket index rather than a modulo, because
    // the present counter can skip values between two rendered frames and a
    // `presents % every == 0` test would silently miss every report whose exact
    // multiple fell in a skipped run -- a cadence that quietly emits nothing.
    uint64_t reportedAtPresent = 0;
    gears::RenderThreadReporter renderThreadReporter;
    uint32_t frameSwaps = 0; // swaps seen while waiting for GEARS_DRAW_FRAME_AT
    // The front buffer address from the swap packet that ends the frame, handed to
    // the renderer so it presents the surface the GUEST named rather than the one a
    // rule of thumb picks.
    uint32_t frontBufferAddress = 0;
    // The same swap packet's fetch constant: the guest's own statement of that
    // buffer's format, size and tiling. See FrameDrawInputs::frontBufferFetch.
    uint32_t frontBufferFetch[6] = {};

    // Predication (Xenos PFP bin mask/select). Bit 0 of a TYPE3 header marks the
    // packet predicated; hardware skips it when (bin_select & bin_mask) == 0.
    // Both registers reset to all-ones, i.e. "everything passes", so a title
    // that never programs them is unaffected. Contract mirrored from
    // extern/xenia src/xenia/gpu/pm4_command_processor_implement.h (bin_select_/
    // bin_mask_ defaults in command_processor.h, the `packet & 1` test, and the
    // SET_BIN_* opcode handlers).
    uint64_t binMask = 0xFFFFFFFFull;
    uint64_t binSelect = 0xFFFFFFFFull;

    // Per-frame census of what predication WOULD change, kept separate from any
    // behavioural use so the effect can be predicted before it is applied.
    std::map<uint32_t, uint64_t> predicatedSeen; // opcode -> predicated packets
    std::map<uint32_t, uint64_t> predicatedSkip; // opcode -> would be skipped
    uint64_t predicateOffPackets = 0;            // any packet while select&mask==0

    // The whole of the CP thread's wall time between frames, split into the
    // three places it can go. Anything unaccounted for is the guest's own
    // execution time, which is the point of the split: it says whether a slow
    // frame is our command processor or the title.
    std::chrono::steady_clock::time_point frameStart = std::chrono::steady_clock::now();
    uint64_t idleUs = 0; // ring empty: waiting for the title to submit
    uint64_t idlePolls = 0;
    uint64_t regWaitUs = 0;   // inside WAIT_REG_MEM
    uint64_t interruptUs = 0; // inside the title's ISR
    uint64_t interrupts = 0;

    void ReportWaitStats()
    {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t frameUs = uint64_t(
            std::chrono::duration_cast<std::chrono::microseconds>(now - frameStart).count());
        frameStart = now;
        lucent::debug("gpu",
                      "frame budget: {} ms total = {} ms ring-empty ({} polls) + {} ms WAIT_REG_MEM"
                      " + {} ms ISR ({} interrupts)",
                      frameUs / 1000, idleUs / 1000, idlePolls, regWaitUs / 1000,
                      interruptUs / 1000, interrupts);
        for (const auto &[addr, stat] : waitStats)
        {
            if (stat.second > 1000)
                lucent::debug("gpu", "  waits on {:#x}: {} times, {} ms", addr, stat.first,
                              stat.second / 1000);
        }
        uint64_t ibTotal = 0;
        uint64_t ibMax = 0;
        for (const auto &[addr, n] : ibCounts)
        {
            ibTotal += n;
            ibMax = std::max(ibMax, n);
        }
        lucent::debug("gpu",
                      "frame packets: {} IB submissions of {} distinct buffers"
                      " (max {} each); ring dwords written {} consumed {}",
                      ibTotal, ibCounts.size(), ibMax, frameWptrAdvance, frameRptrAdvance);
        frameWptrAdvance = frameRptrAdvance = 0;
        gears::ReportGpuRegisterWatch();
        lucent::Line ring;
        ring.add("  ring TYPE3:");
        for (const auto &[op, n] : ringOpcodes)
            ring.add(" {}x{}", OpcodeName(op), n);
        ring.flush_debug("gpu");
        lucent::Line inner;
        inner.add("  IB TYPE3:");
        for (const auto &[op, n] : innerOpcodes)
            inner.add(" {}x{}", OpcodeName(op), n);
        inner.flush_debug("gpu");

        if (lucent::config::flag("CONST_DUMP"))
            lucent::debug(
                "gpu",
                "  constant feed (cumulative): SET_CONSTANT"
                "[alu {} fetch {} bool {} loop {} reg {}] LOAD_ALU_CONSTANT[alu {} fetch {}]"
                " SET_CONSTANT2 {} SET_SHADER_CONSTANTS {} TYPE0[alu {} fetch {}]",
                setConstantByType[0], setConstantByType[1], setConstantByType[2],
                setConstantByType[3], setConstantByType[4], loadAluConstantByType[0],
                loadAluConstantByType[1], setConstant2Packets, setShaderConstantsPackets,
                type0AluWrites, type0FetchWrites);

        lucent::Line pred;
        pred.add("  predication: select {:#x} mask {:#x}; packets seen while OFF {};"
                 " predicated packets",
                 binSelect, binMask, predicateOffPackets);
        for (const auto &[op, n] : predicatedSeen)
            pred.add(" {}x{}(skip {})", OpcodeName(op), n,
                     predicatedSkip.count(op) ? predicatedSkip[op] : 0);
        pred.flush_debug("gpu");
        predicatedSeen.clear();
        predicatedSkip.clear();
        predicateOffPackets = 0;

        if (lucent::config::flag("DRAW_CENSUS"))
        {
            lucent::Line dc;
            dc.add("  draw census (cumulative): {} draws, {} distinct (vs,ps) pairs;", drawsSeen,
                   drawPairs.size());
            for (const auto &[key, n] : drawPairs)
                dc.add(" [{:#018x}/{:#018x}]x{}", key.first, key.second, n);
            dc.flush(lucent::Level::Info, "gpu");
        }

        if (g_shaderCapture.enabled)
        {
            const auto &cap = g_shaderCapture;
            size_t vs = 0, ps = 0;
            uint64_t maxLoads = 0;
            for (const auto &[hash, s] : cap.shaders)
            {
                (s.type == 0 ? vs : ps)++;
                maxLoads = std::max(maxLoads, s.loads);
            }
            lucent::debug(
                "gpu",
                "shader capture: {} IM_LOAD + {} IM_LOAD_IMMEDIATE, "
                "{} distinct microcode payloads ({} vertex, {} pixel), hottest bound {}x, "
                "{} rejected, {} truncated",
                cap.imLoads, cap.imLoadsImmediate, cap.shaders.size(), vs, ps, maxLoads,
                cap.rejected, cap.truncated);
            ShaderCaptureManifest();
        }

        waitStats.clear();
        ringOpcodes.clear();
        innerOpcodes.clear();
        ibCounts.clear();
        idleUs = idlePolls = regWaitUs = interruptUs = interrupts = 0;
    }

    // IM_LOAD:            data[0] = physical address | shaderType(low 2 bits)
    //                     data[1] = (start << 16) | sizeDwords
    // IM_LOAD_IMMEDIATE:  data[0] = shaderType
    //                     data[1] = (start << 16) | sizeDwords
    //                     data[2..] = the microcode itself
    template <typename Fetch>
    void CaptureShaderLoad(uint32_t opcode, Fetch &&fetch, uint32_t usable, uint32_t count)
    {
        auto &cap = g_shaderCapture;
        if (!cap.enabled || usable < 2)
            return;
        const uint32_t word0 = fetch(0);
        const uint32_t startSize = fetch(1);
        const uint32_t start = startSize >> 16;
        const uint32_t sizeDwords = startSize & 0xFFFF;
        const bool immediate = opcode == kOpImLoadImmediate;
        const uint32_t type = immediate ? word0 : (word0 & 3);
        const uint32_t address = immediate ? 0 : (word0 & ~3u);

        // Xenos ucode instructions are 3 dwords; a load that is not a whole
        // number of them, or that starts part-way in, is not something this can
        // reconstruct, and is counted rather than guessed at.
        if (type > 1 || start != 0 || sizeDwords == 0 || sizeDwords % 3 != 0 || sizeDwords > 0x4000)
        {
            ++cap.rejected;
            return;
        }

        std::vector<uint8_t> ucode(size_t(sizeDwords) * 4);
        if (immediate)
        {
            if (usable < 2 + sizeDwords || count < 2 + sizeDwords)
            {
                ++cap.truncated;
                return;
            }
            for (uint32_t i = 0; i < sizeDwords; ++i)
            {
                const uint32_t w = fetch(2 + i);
                ucode[i * 4 + 0] = uint8_t(w >> 24);
                ucode[i * 4 + 1] = uint8_t(w >> 16);
                ucode[i * 4 + 2] = uint8_t(w >> 8);
                ucode[i * 4 + 3] = uint8_t(w);
            }
            ++cap.imLoadsImmediate;
        }
        else
        {
            for (uint32_t i = 0; i < sizeDwords; ++i)
            {
                const uint32_t w = ReadGuest32(address + i * 4);
                ucode[i * 4 + 0] = uint8_t(w >> 24);
                ucode[i * 4 + 1] = uint8_t(w >> 16);
                ucode[i * 4 + 2] = uint8_t(w >> 8);
                ucode[i * 4 + 3] = uint8_t(w);
            }
            ++cap.imLoads;
        }
        RecordBoundShader(type, address, immediate, ucode);
    }

    // Resolve a SET_CONSTANT/LOAD_ALU_CONSTANT (index,type) pair to the base
    // register index of the target constant file. Mirrors Xenia's
    // WriteALURangeFromRing / WriteFetchRangeFromRing / ... in
    // src/xenia/gpu/command_processor.cc. Returns false for an unknown type.
    static bool ConstFileBase(uint32_t type, uint32_t &base)
    {
        switch (type)
        {
        case 0:
            base = kConstBaseAlu;
            return true; // ALU float
        case 1:
            base = kConstBaseFetch;
            return true; // vertex/texture fetch
        case 2:
            base = kConstBaseBool;
            return true; // bool
        case 3:
            base = kConstBaseLoop;
            return true; // loop
        case 4:
            base = kConstBaseRegisters;
            return true; // general registers
        default:
            return false;
        }
    }

    // Loads the sequencer constant files from the command stream, so the bytes
    // the translated shaders read as UBOs (ALU float constants at 0x4000, fetch
    // constants at 0x4800, bool/loop at 0x4900/0x4908) are actually tracked in
    // the register file. Semantics mirror extern/xenia
    // src/xenia/gpu/pm4_command_processor_implement.h
    // (ExecutePacketType3_SET_CONSTANT / _SET_CONSTANT2 / _LOAD_ALU_CONSTANT /
    // _SET_SHADER_CONSTANTS). Uses fetch() rather than HandleType3's 20-word
    // copy because a constant load can carry the whole 1024-dword ALU file.
    template <typename Fetch>
    void TrackConstantLoad(uint32_t opcode, Fetch &&fetch, uint32_t usable, uint32_t count)
    {
        switch (opcode)
        {
        case kOpSetConstant:
        {
            if (usable < 1)
                return;
            const uint32_t offsetType = fetch(0);
            const uint32_t index = offsetType & 0x7FF;
            const uint32_t type = (offsetType >> 16) & 0xFF;
            ++setConstantByType[type & 7];
            uint32_t base;
            if (!ConstFileBase(type, base))
            {
                lucent::warn("gpu", "SET_CONSTANT unknown type {} (offset_type {:#x})", type,
                             offsetType);
                return;
            }
            const uint32_t n = count - 1; // constant dwords after offset_type
            gears::GpuRegisterWriteScope src(
                !gears::GpuRegisterWatchEnabled()
                    ? std::string()
                    : std::format("SET_CONSTANT type {} index {} x{}", type, index, n));
            for (uint32_t i = 0; i < n && (1 + i) < usable; ++i)
                WriteGpuRegister(base + index + i, fetch(1 + i));
            break;
        }
        case kOpLoadAluConstant:
        {
            if (usable < 3)
                return;
            const uint32_t address = fetch(0) & 0x3FFFFFFF;
            const uint32_t offsetType = fetch(1);
            const uint32_t sizeDwords = fetch(2) & 0xFFF;
            const uint32_t index = offsetType & 0x7FF;
            const uint32_t type = (offsetType >> 16) & 0xFF;
            ++loadAluConstantByType[type & 7];
            uint32_t base;
            if (!ConstFileBase(type, base))
            {
                lucent::warn("gpu", "LOAD_ALU_CONSTANT unknown type {} (offset_type {:#x})", type,
                             offsetType);
                return;
            }
            gears::GpuRegisterWriteScope src(
                !gears::GpuRegisterWatchEnabled()
                    ? std::string()
                    : std::format("LOAD_ALU_CONSTANT from {:#x} type {} index {} x{}", address,
                                  type, index, sizeDwords));
            for (uint32_t i = 0; i < sizeDwords; ++i)
                WriteGpuRegister(base + index + i, ReadGuest32(address + i * 4));
            break;
        }
        case kOpSetConstant2:
        case kOpSetShaderConstants:
        {
            // Raw register index, no per-type base (Xenia writes index directly).
            if (usable < 1)
                return;
            const uint32_t index = fetch(0) & 0xFFFF;
            (opcode == kOpSetConstant2 ? setConstant2Packets : setShaderConstantsPackets)++;
            const uint32_t n = count - 1;
            gears::GpuRegisterWriteScope src(
                !gears::GpuRegisterWatchEnabled()
                    ? std::string()
                    : std::format("{} index {:#x} x{}", OpcodeName(opcode), index, n));
            for (uint32_t i = 0; i < n && (1 + i) < usable; ++i)
                WriteGpuRegister(index + i, fetch(1 + i));
            break;
        }
        default:
            break;
        }
    }

    // One-shot verification dump of the constant files. The default selects a
    // repeatedly observed draw profile with a populated vertex stream; the ANY
    // mode accepts the first populated profile. Captured fetch words are decoded
    // through the platform descriptor types rather than a title instruction
    // listing.
    static constexpr uint64_t kHotVertexHash = 0x5363d0746b3ef666ull;

    void DumpConstantFiles(uint32_t drawOpcode)
    {
        if (constDumpDone || !lucent::config::flag("CONST_DUMP"))
            return;

        // Wait for the specific draw whose constants we want to check: the hot
        // pair's vertex shader must be the bound one (unless CONST_DUMP_ANY), and
        // the ALU float file must be populated (skips the movie-phase quad, whose
        // built-in shader uses no float constants).
        const bool anyShader = lucent::config::flag("CONST_DUMP_ANY");
        if (!anyShader && g_shaderCapture.activeVertexHash != kHotVertexHash)
            return;
        uint32_t aluNonZeroGate = 0;
        for (uint32_t i = 0; i < 1024; ++i)
            aluNonZeroGate += g_gpuRegisters[kConstBaseAlu + i] != 0;
        if (aluNonZeroGate == 0)
            return;
        constDumpDone = true;

        lucent::info("gpu", "bound shaders at dump: vertex {:#018x} pixel {:#018x}",
                     g_shaderCapture.activeVertexHash, g_shaderCapture.activePixelHash);
        lucent::info("gpu",
                     "constant dump at {} -- feed census:"
                     " SET_CONSTANT[alu {} fetch {} bool {} loop {} reg {}]"
                     " LOAD_ALU_CONSTANT[alu {} fetch {}] SET_CONSTANT2 {} SET_SHADER_CONSTANTS {}"
                     " TYPE0[alu {} fetch {}]",
                     OpcodeName(drawOpcode), setConstantByType[0], setConstantByType[1],
                     setConstantByType[2], setConstantByType[3], setConstantByType[4],
                     loadAluConstantByType[0], loadAluConstantByType[1], setConstant2Packets,
                     setShaderConstantsPackets, type0AluWrites, type0FetchWrites);

        // Raw fetch register block (0x4800.., 96 dwords = 32 six-dword slots),
        // so the whole file can be inspected. The vertex and texture fetch
        // constants share this file; Xenia reads a vertex fetch at word_0 =
        // (const_index << 1) and a texture fetch at (6 * const_index), so they
        // can alias -- Xenia itself only validates the type at draw time
        // (spirv_shader_translator_fetch.cc:67). Rather than assume where vf0
        // lands, scan for the vertex-typed (type 3) entries and report them.
        {
            lucent::Line raw;
            raw.add("  fetch block:");
            for (uint32_t i = 0; i < 96; ++i)
            {
                if ((i & 5) == 0 && i)
                    raw.add(" |");
                raw.add(" {:08x}", g_gpuRegisters[kConstBaseFetch + i]);
            }
            raw.flush(lucent::Level::Info, "gpu");
        }

        // Classify per 6-dword slot by the type in the slot's first dword: a
        // slot holds EITHER one texture fetch constant (type 2, 6 dwords) OR up
        // to three vertex fetch constants (type 3, 2 dwords each). Classifying by
        // slot avoids misreading a texture's interior dwords (e.g. its size
        // field) as a spurious vertex constant.
        uint32_t vertexConsts = 0, textureSlots = 0;
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            const uint32_t *s = &g_gpuRegisters[kConstBaseFetch + slot * 6];
            const uint32_t slotType = s[0] & 3;
            if (slotType == 2) // xe_gpu_texture_fetch_t
            {
                ++textureSlots;
                const uint32_t base = (s[1] >> 12) << 12;
                const uint32_t pitch = ((s[0] >> 22) & 0x1FF) << 5;
                const uint32_t width = (s[2] & 0x1FFF) + 1;          // size_2d.width
                const uint32_t height = ((s[2] >> 13) & 0x1FFF) + 1; // size_2d.height
                const uint32_t format = s[1] & 0x3F;
                const uint32_t tiled = s[0] >> 31;
                const uint32_t endian = (s[1] >> 6) & 3;
                if (textureSlots <= 12)
                    lucent::info("gpu",
                                 "  texfetch[slot {}] (reg {:#x}): base {:#x} {}x{}"
                                 " pitch {} px format {:#x} tiled {} endian {}",
                                 slot, kConstBaseFetch + slot * 6, base, width, height, pitch,
                                 format, tiled, endian);
            }
            else if (slotType == 3) // xe_gpu_vertex_fetch_t (up to 3 per slot)
            {
                for (uint32_t j = 0; j < 3; ++j)
                {
                    const uint32_t d0 = s[j * 2 + 0];
                    const uint32_t d1 = s[j * 2 + 1];
                    if ((d0 & 3) != 3)
                        continue;
                    ++vertexConsts;
                    const uint32_t byteAddr = (d0 >> 2) << 2;
                    const uint32_t endian = d1 & 3;
                    const uint32_t sizeWords = (d1 >> 2) & 0xFFFFFF;
                    lucent::info("gpu",
                                 "  vfetch const #{} (reg {:#x}): {:#010x} {:#010x} ->"
                                 " base {:#x} size {} words ({} bytes) endian {}",
                                 slot * 3 + j, kConstBaseFetch + slot * 6 + j * 2, d0, d1, byteAddr,
                                 sizeWords, sizeWords * 4, endian);
                }
            }
        }
        lucent::info("gpu",
                     "  fetch file: {} texture slots, {} vertex-fetch constants"
                     " (type 3)",
                     textureSlots, vertexConsts);

        // ALU float constants: 256 vec4 at 0x4000. Count non-zero dwords and show
        // the first few vec4 so the transform matrices are visibly present.
        uint32_t nonZeroAlu = 0;
        for (uint32_t i = 0; i < 1024; ++i)
            if (g_gpuRegisters[kConstBaseAlu + i] != 0)
                ++nonZeroAlu;
        lucent::info("gpu", "  ALU float constants non-zero: {} of 1024 dwords", nonZeroAlu);
        for (uint32_t v = 0; v < 6; ++v)
        {
            const uint32_t *p = &g_gpuRegisters[kConstBaseAlu + v * 4];
            float f[4];
            for (int k = 0; k < 4; ++k)
                std::memcpy(&f[k], &p[k], 4);
            lucent::info("gpu", "  c[{}] = {} {} {} {}  (raw {:#010x} {:#010x} {:#010x} {:#010x})",
                         v, f[0], f[1], f[2], f[3], p[0], p[1], p[2], p[3]);
        }

        // Bool/loop files.
        uint32_t nonZeroBool = 0, nonZeroLoop = 0;
        for (uint32_t i = 0; i < 8; ++i)
            nonZeroBool += g_gpuRegisters[kConstBaseBool + i] != 0;
        for (uint32_t i = 0; i < 32; ++i)
            nonZeroLoop += g_gpuRegisters[kConstBaseLoop + i] != 0;
        lucent::info("gpu", "  bool file non-zero dwords: {}/8; loop file non-zero: {}/32",
                     nonZeroBool, nonZeroLoop);

        // Raw register-file snapshot for the offline system-constants verifier
        // (tools/system_constants). It reloads these dwords into a Xenia
        // RegisterFile and runs Xenia's own draw_util + SystemConstants
        // derivation against them, so the NDC/index-endian bytes it produces are
        // checked against the actual register state of this draw rather than an
        // assumed one. The whole 0x8000-dword space is written little-endian;
        // Xenia's RegisterFile only spans the first 0x5003, which is a prefix.
        {
            namespace fs = std::filesystem;
            const char *dir = std::getenv("GEARS_CONST_DUMP_DIR");
            fs::path outdir = dir ? fs::path(dir) : fs::path("scratch/bin");
            std::error_code ec;
            fs::create_directories(outdir, ec);
            const fs::path out = outdir / "regfile_hotpair.bin";
            std::ofstream f(out, std::ios::binary);
            if (f)
            {
                f.write(reinterpret_cast<const char *>(g_gpuRegisters.data()),
                        std::streamsize(g_gpuRegisters.size() * sizeof(uint32_t)));
                lucent::info("gpu", "  wrote register-file snapshot ({} dwords) to {}",
                             g_gpuRegisters.size(), out.string());
            }
            else
            {
                lucent::warn("gpu", "  could not open {} for register dump", out.string());
            }
        }
    }

    // ----------------------------------------------------------------------
    // Hot-pair draw-parameter capture.
    //
    // At a DRAW_INDX / DRAW_INDX_2 whose bound shaders are the hot pair, capture
    // the full draw parameters and the geometry source, so a pipeline could be
    // fed. Packet layout mirrors extern/xenia
    // src/xenia/gpu/pm4_command_processor_implement.h
    // (ExecutePacketType3_DRAW_INDX / _DRAW_INDX_2 / ExecutePacketType3Draw) and
    // the bitfields in src/xenia/gpu/registers.h (VGT_DRAW_INITIATOR,
    // VGT_DMA_SIZE) and xenos.h (SourceSelect, IndexFormat, PrimitiveType,
    // Endian, xe_gpu_vertex_fetch_t):
    //
    //   DRAW_INDX:   data[0]=viz token, data[1]=VGT_DRAW_INITIATOR,
    //                then if source_select==kDMA: data[2]=VGT_DMA_BASE,
    //                data[3]=VGT_DMA_SIZE.
    //   DRAW_INDX_2: no viz token; data[0]=VGT_DRAW_INITIATOR, then DMA words.
    //
    //   VGT_DRAW_INITIATOR: prim_type[0:5], source_select[6:7], major_mode[8:9],
    //                       index_size[11] (0=int16,1=int32), num_indices[16:31].
    //   VGT_DMA_SIZE:       num_words[0:23], swap_mode[30:31].
    //
    // The draw packet does not carry its vertex stream. Captured descriptor
    // state for this diagnostic profile identifies fetch slot 95 with a
    // 12-dword stride; its first word supplies the stream base.
    static constexpr uint32_t kHotVertexFetchIndex = 95;
    static constexpr uint32_t kHotVertexStrideDwords = 12;

    // Accumulate one DRAW_INDX/_2 for the whole-frame backend: snapshot the
    // register file live at this draw (constants/fetch/initiator change between
    // draws) plus the bound shader pair and the index-buffer parameters. Nothing
    // is invented -- shaders come from the capture map, geometry from the fetch
    // constants in the snapshot, indices from the packet's DMA words.
    void CaptureFrameDraw(uint32_t opcode, const uint32_t *raw, uint32_t usable, uint32_t initiator)
    {
        if (!frameProbeCapture.AcceptDraws(frameRenderDone))
        {
            ++drawsAfterFrameDone;
            return;
        }
        const uint64_t vsHash = g_shaderCapture.activeVertexHash;
        const uint64_t psHash = g_shaderCapture.activePixelHash;
        auto vsIt = g_shaderCapture.shaders.find(vsHash);
        auto psIt = g_shaderCapture.shaders.find(psHash);
        ++drawsOffered;
        if (vsIt == g_shaderCapture.shaders.end() || psIt == g_shaderCapture.shaders.end() ||
            vsIt->second.type != 0 || psIt->second.type != 1 || vsIt->second.ucode.empty() ||
            psIt->second.ucode.empty())
        {
            // "Skip honestly" was the intent, but skipping SILENTLY is not
            // honest: this and the three returns below discard a draw the guest
            // issued and, until they were counted, a frame that lost two thirds
            // of its geometry here looked exactly like a frame that had none to
            // lose. The oracle records ~2141 draws on a gameplay frame where we
            // record ~800, at the same guest frame rate and the same resolve
            // count, and these returns were the first place that gap could be
            // hiding (catalog #77).
            ++drawsNoShaderPair;
            return;
        }

        const uint32_t sourceSelect = (initiator >> 6) & 0x3;
        const uint32_t indexSizeBit = (initiator >> 11) & 0x1; // 0=int16,1=int32
        const uint32_t numIndices = (initiator >> 16) & 0xFFFF;
        if (numIndices == 0)
        {
            ++drawsZeroIndices;
            return;
        }
        const uint32_t indexSizeBytes = indexSizeBit ? 4u : 2u;

        bool indexed = false;
        uint32_t indexGuestBase = 0;
        uint32_t indexEndian = 0;
        if (sourceSelect == 0) // kDMA: index buffer, VGT_DMA_BASE follows the initiator
        {
            indexed = true;
            const uint32_t initiatorIdx = (opcode == 0x22) ? 1u : 0u;
            const uint32_t baseIdx = initiatorIdx + 1;
            const uint32_t sizeIdx = initiatorIdx + 2;
            if (usable <= sizeIdx)
            {
                // A partial DMA packet cannot name the byte order of its
                // indices. Refuse the draw rather than silently choosing one.
                ++drawsImmediateIndex;
                return;
            }
            indexGuestBase = raw[baseIdx] & ~(indexSizeBytes - 1);
            indexEndian = (raw[sizeIdx] >> 30) & 3;
        }
        else if (sourceSelect == 2) // kAutoIndex: sequential 0..num-1, no index buffer
        {
            indexed = false;
        }
        else
        {
            // kImmediate: the indices ride in the packet itself. Not decoded
            // yet -- and counted rather than dropped in silence, because "we
            // do not implement this" and "the title never uses it" are the same
            // number of log lines otherwise.
            ++drawsImmediateIndex;
            return;
        }

        gears::FrameDrawItem item;
        // Snapshot only when a register has actually changed since the last
        // draw; otherwise every draw shares the previous snapshot. This was
        // ~90 MB of memcpy per gameplay frame.
        if (g_registersDirty || !g_registerSnapshot)
        {
            // Renderer consumers alias this as Xenia's RegisterFile, whose
            // authoritative size is 0x5003. The command processor keeps 0x8000
            // dwords for MMIO bookkeeping, but copying the unused tail on every
            // changed draw is pure bandwidth.
            g_registerSnapshot = std::make_shared<std::vector<uint32_t>>(
                g_gpuRegisters.begin(), g_gpuRegisters.begin() + gears::kGpuRegisterSnapshotDwords);
            g_registersDirty = false;
            ++g_registerSnapshots;
        }
        item.registerFile = g_registerSnapshot;
        item.vsUcode = vsIt->second.ucode.data();
        item.vsUcodeSize = vsIt->second.ucode.size();
        item.vsHash = vsHash;
        item.psUcode = psIt->second.ucode.data();
        item.psUcodeSize = psIt->second.ucode.size();
        item.psHash = psHash;
        item.primType = initiator & 0x3F;
        item.indexCount = numIndices;
        item.indexed = indexed;
        item.indexIs32 = indexSizeBit != 0;
        item.indexEndian = indexEndian;
        item.indexGuestBase = indexGuestBase;
        frameDraws.push_back(std::move(item));
        // For GEARS_GPU_REG_WATCH: which draw of this frame a later register
        // write precedes. A bulk census of a SHARED constant register cannot
        // say which shader a value was meant for; the draw ordinal can.
        g_regWatchDrawOrdinal = uint32_t(frameDraws.size());
    }

    // Diagnostic control (GEARS_CP_STALL_MS=N): block the command-processor
    // thread for N ms at the first swap, doing nothing else. This isolates
    // "a long stall on the CP thread" from "the capture touched guest state":
    // if the pure stall reproduces the guest's quit path and the capture-free
    // run does not, the stall is the mechanism.
    bool cpStallDone = false;
    void TriggerCpStall()
    {
        const long ms = lucent::config::number("CP_STALL_MS", 0);
        if (cpStallDone || ms <= 0)
            return;
        cpStallDone = true;
        lucent::info("gpu", "cp-stall: blocking the command processor for {} ms", ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        lucent::info("gpu", "cp-stall: resumed");
    }

    // At the frame boundary, render every accumulated draw into the renderer's
    // persistent target. GEARS_DRAW_FRAME_COUNT=N renders N consecutive frames
    // from GEARS_DRAW_FRAME_AT instead of one, which is how the per-frame cost
    // of a WARM renderer is measured -- the first frame pays for translating
    // every shader and building every pipeline, and says nothing about the
    // steady state.
    void SetFrontBuffer(uint32_t address) { frontBufferAddress = address; }
    void SetFrontBufferFetch(const uint32_t *six)
    {
        std::memcpy(frontBufferFetch, six, sizeof(frontBufferFetch));
    }

    void TriggerFrameRender()
    {
        const bool pendingProbe = gears::PendingGraphicsProbeRequest() != 0;
        if (frameProbeCapture.ArmNextFrameAtBoundary(frameRenderDone, pendingProbe))
        {
            drawsOffered = drawsNoShaderPair = drawsZeroIndices = 0;
            drawsImmediateIndex = drawsAfterFrameDone = 0;
            frameDraws.clear();
            return;
        }
        if (!frameProbeCapture.AcceptDraws(frameRenderDone) || frameDraws.empty())
            return;
        // DRAW_FRAME_AT selects by index; DRAW_FRAME_MIN_DRAWS selects content
        // because load timing makes indices non-repeatable (catalog #89). It
        // renders after the threshold, matching GEARS_ORACLE_DUMP_MIN_DRAWS.
        const long minDraws = lucent::config::number("DRAW_FRAME_MIN_DRAWS", 0);
        const long target = lucent::config::number("DRAW_FRAME_AT", 0);
        const bool probeRequested = frameProbeCapture.Requested(pendingProbe);
        const bool diagnosticProbe = gears::ShouldBypassFrameSelection(
            probeRequested, frameRenderDone, minDraws > 0 && !contentGateOpen,
            minDraws <= 0 && long(frameSwaps) < target);
        if (!diagnosticProbe && minDraws > 0 && !contentGateOpen)
        {
            ++contentScans;
            contentBusiest = std::max(contentBusiest, uint32_t(frameDraws.size()));
            const uint64_t minGuestFrame = uint64_t(
                std::max<long>(0, lucent::config::number("DRAW_FRAME_MIN_GUEST_FRAME", 0)));
            if (frameSwaps < minGuestFrame)
            {
                if (frameSwaps == 0 || frameSwaps % 600 == 0)
                    lucent::info("gpu",
                                 "guest-draw: {} frames scanned; content"
                                 " selector is ineligible until guest frame {}. NOTHING"
                                 " has been rendered or captured.",
                                 contentScans, minGuestFrame);
                drawsOffered = drawsNoShaderPair = drawsZeroIndices = 0;
                drawsImmediateIndex = drawsAfterFrameDone = 0;
                ++frameSwaps;
                frameDraws.clear();
                return;
            }
            if (contentArmed && contentWait > 0)
            {
                // GEARS_DRAW_FRAME_AFTER_GAMEPLAY frames of slack. The FIRST
                // gameplay frame is a fade from black -- the console's own
                // capture of it resolves its post-chain output and its front
                // buffer entirely zero -- so a comparison taken there compares
                // two black frames and says nothing. The oracle fork takes the
                // same offset (GEARS_ORACLE_DUMP_AFTER_GAMEPLAY).
                --contentWait;
                drawsOffered = drawsNoShaderPair = drawsZeroIndices = 0;
                drawsImmediateIndex = drawsAfterFrameDone = 0;
                ++frameSwaps;
                frameDraws.clear();
                return;
            }
            if (contentArmed)
            {
                // The content requirement, if one was asked for. Parsed once:
                // <16-hex shader hash>[:<minimum vertices>], matched against a
                // draw's VERTEX or PIXEL shader -- the caller knows one of the
                // two and should not have to know which.
                struct Need
                {
                    uint64_t hash = 0;
                    uint32_t verts = 0;
                    uint32_t maxVerts = 0xFFFFFFFFu;
                };
                static const Need need = []
                {
                    Need n;
                    const std::string &t = lucent::config::text("DRAW_FRAME_NEEDS");
                    if (t.empty())
                        return n;
                    const size_t colon = t.find(':');
                    const std::string hex = t.substr(0, colon);
                    char *end = nullptr;
                    n.hash = std::strtoull(hex.c_str(), &end, 16);
                    if (end == hex.c_str() || n.hash == 0)
                    {
                        lucent::warn("gpu",
                                     "GEARS_DRAW_FRAME_NEEDS: cannot parse"
                                     " '{}' as a 16-hex shader hash. NO content requirement"
                                     " is applied and the next frame will be captured",
                                     hex);
                        n.hash = 0;
                        return n;
                    }
                    if (colon != std::string::npos)
                    {
                        // <min>[-<max>]. The upper bound exists because the case
                        // this was built for is identified by a vertex count
                        // DIFFERENT from the one that works: the mask pass's
                        // marking draw is 30,876 vertices when it rasterises and
                        // 19,776 when it clips away, so ">= N" would happily
                        // select the working frame every time.
                        const char *p = t.c_str() + colon + 1;
                        char *e2 = nullptr;
                        n.verts = uint32_t(std::strtoul(p, &e2, 10));
                        if (e2 && *e2 == '-')
                            n.maxVerts = uint32_t(std::strtoul(e2 + 1, nullptr, 10));
                    }
                    lucent::info("gpu",
                                 "guest-draw: the capture also REQUIRES a"
                                 " draw of shader {:#018x} with {}..{} vertices; frames"
                                 " without one are skipped, not captured",
                                 n.hash, n.verts,
                                 n.maxVerts == 0xFFFFFFFFu ? std::string("any")
                                                           : std::to_string(n.maxVerts));
                    return n;
                }();
                uint32_t matchVerts = 0;
                bool shaderPresent = false;
                if (need.hash != 0)
                {
                    for (const auto &fd : frameDraws)
                    {
                        if (fd.vsHash != need.hash && fd.psHash != need.hash)
                            continue;
                        shaderPresent = true;
                        contentBestVerts = std::max(contentBestVerts, fd.indexCount);
                        if (fd.indexCount >= need.verts && fd.indexCount <= need.maxVerts)
                            matchVerts = std::max(matchVerts, fd.indexCount);
                    }
                    contentShaderSeen += shaderPresent ? 1 : 0;
                    if (matchVerts == 0)
                    {
                        // HELD, and the negative carries its denominators: how
                        // many frames were looked at, how many had the shader at
                        // all, and the biggest one seen. "The draw never came"
                        // and "nobody looked" are otherwise the same silence.
                        ++contentHeld;
                        if (contentHeld == 1 || contentHeld % 60 == 0)
                            lucent::info("gpu",
                                         "guest-draw: {} frame(s) past the"
                                         " draw-count gate held for content -- {} of them"
                                         " carried shader {:#018x} at all, and the biggest"
                                         " such draw was {} vertices against the {}..{}"
                                         " required. NOTHING has been captured",
                                         contentHeld, contentShaderSeen, need.hash,
                                         contentBestVerts, need.verts,
                                         need.maxVerts == 0xFFFFFFFFu
                                             ? std::string("any")
                                             : std::to_string(need.maxVerts));
                        drawsOffered = drawsNoShaderPair = drawsZeroIndices = 0;
                        drawsImmediateIndex = drawsAfterFrameDone = 0;
                        ++frameSwaps;
                        frameDraws.clear();
                        return;
                    }
                }
                // GEARS_DRAW_FRAME_CAMERA=<oracle constants>[:<near>[:<rot>[:<base>]]]
                // THE ONLY PAIRING KEY THAT SURVIVES. Frame index, draw ordinal
                // and this very content predicate have each been used to pair
                // the two emulators and each silently compared different moments
                // -- catalog #91's shadow-volume draws looked catastrophic for
                // several sessions on that basis and agree to 1% once the
                // viewpoints are matched. The view-projection is GUEST data that
                // both emulators carry unchanged, so it names the moment itself.
                // Read out of the draw's own register-file snapshot, so it is
                // the value THAT DRAW used.
                struct Cam
                {
                    bool on = false;
                    uint64_t vs = 0;
                    // near is now the TRANSLATION tolerance as a FRACTION of the
                    // translation row's own magnitude, so it means the same
                    // thing wherever in the level the camera is. 0.013 is ~10
                    // units out of the ~757 this title's row carries, and 10 is
                    // CHOSEN BECAUSE IT IS WHAT THE LEVEL'S MOTION ALLOWS: a run
                    // at 10 matched (at 9.29), and a run at 2 never matched at
                    // all -- 16,140 frames held, closest 2.43. Tightening the
                    // position tolerance does not buy a better pairing, it buys
                    // NO pairing, because the guest walks past the viewpoint and
                    // does not return. The discrimination therefore has to come
                    // from the rotation limit below, which was previously absent
                    // rather than loose.
                    double near = 0.013;
                    // ...and rotation gets its own, in absolute units, because
                    // those rows are bounded by ~1.65 and a degree of rotation
                    // moves a component by about 0.017.
                    double rotNear = 0.01;
                    uint32_t constBase = 230;
                    float m[16] = {};
                };
                static const Cam cam = []
                {
                    Cam c;
                    const std::string &t = lucent::config::text("DRAW_FRAME_CAMERA");
                    if (t.empty())
                        return c;
                    // <file>[:<translation fraction>[:<rotation absolute>
                    //        [:<view-projection constant base>]]]
                    std::string path = t;
                    const size_t c1 = t.find(':', 2);
                    if (c1 != std::string::npos)
                    {
                        path = t.substr(0, c1);
                        c.near = std::strtod(t.c_str() + c1 + 1, nullptr);
                        const size_t c2 = t.find(':', c1 + 1);
                        if (c2 != std::string::npos)
                        {
                            c.rotNear = std::strtod(t.c_str() + c2 + 1, nullptr);
                            const size_t c3 = t.find(':', c2 + 1);
                            if (c3 != std::string::npos)
                                c.constBase =
                                    uint32_t(std::strtoul(t.c_str() + c3 + 1, nullptr, 10));
                        }
                    }
                    std::ifstream f(path);
                    if (!f)
                    {
                        lucent::warn("gpu",
                                     "GEARS_DRAW_FRAME_CAMERA: cannot open"
                                     " '{}'. NO camera gate is running and the capture"
                                     " falls back to the content predicate, which pairs"
                                     " the two sides badly",
                                     path);
                        return c;
                    }
                    int found = 0;
                    for (std::string line; std::getline(f, line);)
                    {
                        int idx = 0;
                        float v[4] = {};
                        if (std::sscanf(line.c_str(), "c[%d]=(%f, %f, %f, %f)", &idx, &v[0], &v[1],
                                        &v[2], &v[3]) != 5)
                            continue;
                        if (idx < int(c.constBase) || idx > int(c.constBase + 3))
                            continue;
                        std::memcpy(&c.m[(idx - int(c.constBase)) * 4], v, sizeof(v));
                        ++found;
                    }
                    if (found != 4)
                    {
                        lucent::warn("gpu",
                                     "GEARS_DRAW_FRAME_CAMERA: '{}' has"
                                     " {} of the 4 view-projection rows c{}..c{}. NO"
                                     " camera gate is running",
                                     path, found, c.constBase, c.constBase + 3);
                        return c;
                    }
                    c.on = true;
                    c.vs = need.hash;
                    lucent::info("gpu",
                                 "guest-draw: the capture also REQUIRES a"
                                 " view-projection c{}..c{} within {} of the console's,"
                                 " read from {} and compared against the register file"
                                 " of every draw of {:#018x}",
                                 c.constBase, c.constBase + 3, c.near, path, c.vs);
                    return c;
                }();
                if (cam.on && cam.vs != 0)
                {
                    double bestDist = 1e30, bestRot = 0.0, bestTransRel = 0.0;
                    for (const auto &fd : frameDraws)
                    {
                        if (fd.vsHash != cam.vs || !fd.registerFile)
                            continue;
                        const uint32_t *R = fd.registerFile->data();
                        // ROTATION AND TRANSLATION ARE MEASURED SEPARATELY, and
                        // that is the whole point of this loop rather than a
                        // refinement. A single max-abs over all 16 components
                        // CANNOT CONSTRAIN ORIENTATION AT ALL: the three
                        // rotation rows are bounded by about 1.65 in this title
                        // while the translation row reaches 757, so a threshold
                        // large enough for the translation row (10, say) is
                        // larger than the biggest difference a rotation row can
                        // physically produce -- 3.3, the camera turned right
                        // around. The old metric matched position to ~1% and
                        // accepted ANY orientation, which is why a frame that
                        // "MATCHED at 9.29" correlated with the console's at
                        // 0.376 where a real match scores 0.94 (catalog #87).
                        // No single threshold can fix that: orientation needs
                        // ~0.05 and the translation row can never meet it.
                        double dRot = 0.0, dTrans = 0.0, transScale = 0.0;
                        for (int i = 0; i < 16; ++i)
                        {
                            float f;
                            std::memcpy(&f, &R[0x4000 + cam.constBase * 4 + i], sizeof f);
                            const double diff = std::fabs(f - cam.m[i]);
                            if (i < 12)
                                dRot = std::max(dRot, diff);
                            else
                            {
                                dTrans = std::max(dTrans, diff);
                                transScale = std::max(transScale, double(std::fabs(cam.m[i])));
                            }
                        }
                        // The translation row is judged RELATIVE to its own
                        // magnitude, so the threshold means the same thing
                        // wherever in the level the camera stands.
                        const double dTransRel = transScale > 1e-6 ? dTrans / transScale : dTrans;
                        // One number for the caller, but only after each half
                        // has been scaled to its own threshold: a draw passes
                        // when BOTH are within their limits, so the combined
                        // distance is the worse of the two in units of "how
                        // many thresholds away".
                        const double d = std::max(dRot / cam.rotNear, dTransRel / cam.near);
                        if (d < bestDist)
                        {
                            bestDist = d;
                            bestRot = dRot;
                            bestTransRel = dTransRel;
                        }
                    }
                    contentCamBest = std::min(contentCamBest, bestDist);
                    // bestDist is already in units of "how many thresholds
                    // away", so the pass test is against 1.0 and the two halves
                    // are reported separately -- a hold that does not say WHICH
                    // half failed is what let an orientation-blind metric look
                    // like a working gate for three runs.
                    if (bestDist > 1.0)
                    {
                        // The negative carries the distance AND the best seen so
                        // far, because "the camera never got there" and "no draw
                        // of that shader ran" are different failures and a bare
                        // hold cannot tell them apart.
                        ++contentCamHeld;
                        if (contentCamHeld == 1 || contentCamHeld % 60 == 0)
                            lucent::info(
                                "gpu",
                                "guest-draw: {} frame(s) held for"
                                " the CAMERA -- this frame's closest draw of"
                                " {:#018x} is {}, the closest any frame has come"
                                " is {}, and 1.00 is required. NOTHING has been"
                                " captured",
                                contentCamHeld, cam.vs,
                                bestDist > 1e29 ? std::string("no such draw")
                                                : std::format("rotation {:.4f}/{} and"
                                                              " translation {:.5f}/{} -> {:.2f}"
                                                              " thresholds away",
                                                              bestRot, cam.rotNear, bestTransRel,
                                                              cam.near, bestDist),
                                contentCamBest > 1e29 ? std::string("nothing")
                                                      : std::format("{:.2f}", contentCamBest));
                        drawsOffered = drawsNoShaderPair = drawsZeroIndices = 0;
                        drawsImmediateIndex = drawsAfterFrameDone = 0;
                        ++frameSwaps;
                        frameDraws.clear();
                        return;
                    }
                    lucent::info("gpu",
                                 "guest-draw: CAMERA MATCHED -- rotation"
                                 " {:.4f} (limit {}) and translation {:.5f} of its own"
                                 " magnitude (limit {}), i.e. {:.2f} thresholds, after {}"
                                 " frame(s) held. THIS matched frame is the capture; any"
                                 " remaining temporal residual is world/animation state,"
                                 " not an intentional one-frame capture delay",
                                 bestRot, cam.rotNear, bestTransRel, cam.near, bestDist,
                                 contentCamHeld);
                }
                contentGateOpen = true;
                lucent::info("gpu",
                             "guest-draw: frame {} is the capture -- it"
                             " follows the first frame with >= {} draws, after {} frames"
                             " scanned{}",
                             frameSwaps, minDraws, contentScans,
                             need.hash == 0
                                 ? std::string()
                                 : std::format(", and is the first of {} frame(s) checked"
                                               " to carry a draw of {:#018x} with {}"
                                               " vertices ({}..{} required)",
                                               contentHeld + 1, need.hash, matchVerts, need.verts,
                                               need.maxVerts == 0xFFFFFFFFu
                                                   ? std::string("any")
                                                   : std::to_string(need.maxVerts)));
            }
            else
            {
                if (long(frameDraws.size()) >= minDraws)
                {
                    contentArmed = true;
                    contentWait = uint32_t(
                        std::max<long>(0, lucent::config::number("DRAW_FRAME_AFTER_GAMEPLAY", 0)));
                    lucent::info("gpu",
                                 "guest-draw: frame {} has {} draws (>= {})"
                                 " after {} frames scanned; capturing {} frame(s) later",
                                 frameSwaps, frameDraws.size(), minDraws, contentScans,
                                 contentWait + 1);
                }
                else if (contentScans == 1 || contentScans % 300 == 0)
                {
                    // The periodic NEGATIVE with its denominator: a run that
                    // never reaches the threshold must not be silent, or it
                    // reads as a run that rendered gameplay and dumped nothing.
                    lucent::info("gpu",
                                 "guest-draw: {} frames scanned, none with"
                                 " >= {} draws yet (busiest so far: {} draws). NOTHING has"
                                 " been rendered or captured.",
                                 contentScans, minDraws, contentBusiest);
                }
                if (target > 0)
                    lucent::warn("gpu",
                                 "GEARS_DRAW_FRAME_AT={} is set but"
                                 " GEARS_DRAW_FRAME_MIN_DRAWS={} selects the frame; the"
                                 " index is IGNORED",
                                 target, minDraws);
                drawsOffered = drawsNoShaderPair = drawsZeroIndices = 0;
                drawsImmediateIndex = drawsAfterFrameDone = 0;
                ++frameSwaps;
                frameDraws.clear();
                return;
            }
        }
        if (!diagnosticProbe && minDraws <= 0 && long(frameSwaps) < target)
        {
            // Info, not debug: this line IS the draws-per-frame profile used to
            // choose which frame to capture. One line per frame, no other cost.
            lucent::info("gpu",
                         "guest-draw: frame {} has {} draws of {} the guest"
                         " issued (dropped: {} no shader pair, {} zero indices, {}"
                         " immediate-index, {} after frame done) (waiting for {})",
                         frameSwaps, frameDraws.size(), drawsOffered, drawsNoShaderPair,
                         drawsZeroIndices, drawsImmediateIndex, drawsAfterFrameDone, target);
            drawsOffered = drawsNoShaderPair = drawsZeroIndices = 0;
            drawsImmediateIndex = drawsAfterFrameDone = 0;
            ++frameSwaps;
            frameDraws.clear();
            return;
        }
        if (gears::FrameMayWriteCapture(diagnosticProbe))
            ++framesRendered;
        // GEARS_DRAW_FRAME_COUNT=0 (the default) renders EVERY frame from
        // GEARS_DRAW_FRAME_AT onward -- the live path. A positive count stops
        // after that many, which is what the capture and measurement runs use.
        const long frameCount = lucent::config::number("DRAW_FRAME_COUNT", 0);
        if (gears::FrameMayWriteCapture(diagnosticProbe) && frameCount > 0 &&
            framesRendered >= frameCount)
            frameRenderDone = true;

        // Keep per-frame accounting available on demand without making the
        // default logger part of the renderer's steady-state workload.
        lucent::debug("gpu",
                      "guest-draw: frame {} kept {} of {} draws the guest"
                      " issued (dropped: {} no shader pair, {} zero indices, {}"
                      " immediate-index, {} after frame done)",
                      frameSwaps, frameDraws.size(), drawsOffered, drawsNoShaderPair,
                      drawsZeroIndices, drawsImmediateIndex, drawsAfterFrameDone);
        drawsOffered = drawsNoShaderPair = drawsZeroIndices = 0;
        drawsImmediateIndex = drawsAfterFrameDone = 0;

        gears::FrameDrawInputs in;
        in.frontBufferAddress = frontBufferAddress;
        std::memcpy(in.frontBufferFetch, frontBufferFetch, sizeof(frontBufferFetch));
        // Null until the title has actually uploaded one: passing the linear
        // default instead would make "no ramp programmed" and "a ramp that
        // happens to be linear" indistinguishable downstream.
        in.gammaRamp = (g_gammaRamp.directWrites + g_gammaRamp.seqWrites + g_gammaRamp.pwlWrites)
                           ? g_gammaRamp.table
                           : nullptr;
        in.guestBase = gears::Memory().Base();
        // Mirror a generous window of low guest physical memory so per-draw
        // vertex fetches resolve. Vertex/index buffers observed so far live in
        // the low MBs; a draw whose fetch base exceeds this reads zero and is
        // reported by the backend as empty geometry rather than faked.
        // How much guest physical memory the shaders can FETCH from -- the whole
        // console window, because a vertex fetch constant may name any of it.
        // This was 64 MiB, and that was the missing world (catalog #30): an Act 1
        // frame fetches vertices as high as 0xecf926c (237 MiB), 606 of its 722
        // draws fetched past the mirror, and a fetch past it reads ZERO, so every
        // primitive collapsed to the origin and was destroyed at clipping.
        //
        // The renderer does not copy this much per frame: it uploads only the
        // ranges the frame's draws actually fetch (a few MiB), so the span costs
        // address space rather than bandwidth.
        in.guestPhysicalMirrorBytes = gears::kGuestPhysicalMirrorBytes;
        // Textures live anywhere in the console's 512 MiB of physical RAM,
        // which is mapped at the 0x0 alias; the texture decoder reads it
        // directly (bounds-checked) rather than through the SSBO mirror.
        in.guestWindowBytes = gears::kGuestPhysicalMirrorBytes;
        in.draws = std::move(frameDraws);
        in.probe = probeRequested;
        // A capture run reports on its last frame. A live run reports never,
        // unless GEARS_DRAW_FRAME_REPORT_EVERY=N asks for a periodic census and
        // screenshot -- it costs ~40 ms, so it is a visible hitch by design.
        const long reportEvery = lucent::config::number("DRAW_FRAME_REPORT_EVERY", 0);
        // Use THIS packet's sequence: the global VdSwap count can already be newer
        // when the CP catches up, assigning two packets one identity. This is also
        // the ID used to reject stale packets and drive presentation.
        const uint64_t guestPresents = lastSwapSequence;
        const bool reportCadenceElapsed =
            reportEvery > 0 && guestPresents / uint64_t(reportEvery) > reportedAtPresent;
        in.report = !diagnosticProbe &&
                    (frameCount > 0 ? framesRendered >= frameCount : reportCadenceElapsed);
        if (in.report && frameCount <= 0 && reportEvery > 0)
            reportedAtPresent = guestPresents / uint64_t(reportEvery);
        // Carry guest-present identity through every frame; scan-out must not mint its own clock.
        in.sequence = long(guestPresents);

        // GEARS_DRAW_FRAME_DUMP=<path>: write this frame's whole draw stream to a
        // file that tools/frame_replay renders offline. Reaching a gameplay frame
        // costs a scripted 200-second walk through the menus, and two such runs
        // never land on the same game moment -- so a captured frame, replayable
        // in a second, is the only way to compare two renderer arms on identical
        // input. Written for the REPORTED frame only, so a live run leaves one
        // capture rather than one per frame.
        //
        // GEARS_DRAW_FRAME_DUMP_SKINNED self-selects the first frame that SUBMITS
        // a skinned character. It deliberately does not require visibility,
        // because whether the submitted character survives to the screen is the
        // defect that capture exists to investigate. See frame_content.h.
        const std::string &dumpPath = lucent::config::text("DRAW_FRAME_DUMP");
        if (gears::FrameMayWriteCapture(diagnosticProbe) && !dumpPath.empty() && !frameDumpWritten)
        {
            if (lucent::config::flag("DRAW_FRAME_DUMP_SKINNED"))
            {
                const gears::SkinnedFrameCensus c = gears::ScanForSkinnedCharacter(
                    in, uint32_t(lucent::config::number("SKINNED_MIN_INDICES",
                                                        gears::kDefaultSkinnedMinIndices)));
                ++skinnedScans;
                skinnedBestIndices = std::max(skinnedBestIndices, c.largestSkinnedIndices);
                skinnedBestDraws = std::max(skinnedBestDraws, c.skinnedDraws);
                if (c.Passed())
                {
                    lucent::info("gpu",
                                 "guest-draw: frame {} has a skinned character after {}"
                                 " scans; capturing it",
                                 frameSwaps, skinnedScans);
                    gears::ReportSkinnedFrameCensus(c);
                    frameDumpWritten = gears::WriteFrameCapture(dumpPath.c_str(), in);
                }
                else if (skinnedScans == 1 || skinnedScans % 300 == 0)
                {
                    // The periodic negative, with what the scan HAS seen. A run
                    // that never finds a character has to be distinguishable
                    // from one whose scan is broken.
                    lucent::info("gpu",
                                 "guest-draw: {} frames scanned, none has"
                                 " submitted a skinned character yet (best so far: {}"
                                 " skinned draws in a frame, largest {} indices)",
                                 skinnedScans, skinnedBestDraws, skinnedBestIndices);
                    gears::ReportSkinnedFrameCensus(c);
                }
            }
            else if (in.report)
            {
                frameDumpWritten = gears::WriteFrameCapture(dumpPath.c_str(), in);
            }
        }

        // A CAPTURE OR MEASUREMENT RUN RENDERS IN LINE, a live run does not.
        // GEARS_DRAW_FRAME_COUNT>0 means "render exactly these N frames and report
        // on them" -- dropping one because a thread was busy would make the run
        // measure something other than what it asked for, and the guest stalling
        // is irrelevant when the point is the render itself.
        if (frameCount > 0)
        {
            lucent::info("gpu", "guest-draw: rendering whole frame ({} draws captured)",
                         in.draws.size());
            const auto t0 = std::chrono::steady_clock::now();
            gears::RenderFrameWithGraphicsProbe(in);
            const uint64_t ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - t0)
                                             .count());
            if (diagnosticProbe)
                lucent::info("gpu",
                             "guest-draw: HTTP probe rendered without"
                             " consuming the {}/{} selected capture frame(s); RenderFrame"
                             " blocked the command processor for {} ms",
                             framesRendered, frameCount, ms);
            else
                lucent::info("gpu",
                             "guest-draw: frame {} of {}: RenderFrame blocked"
                             " the command processor for {} ms",
                             framesRendered, frameCount, ms);
        }
        else
        {
            // LIVE: hand the draw list to the render thread and return to the
            // guest. Rendering inside the guest's VdSwap cost it the whole frame
            // time -- VdSwap fell from 29.9 to 17.9 fps the moment gameplay
            // started, and the audio pump fell behind with it, because it is
            // paced by a hand-off with a guest thread.
            const bool taken = gears::SubmitFrameForRender(std::move(in));
            (void)taken;
            renderThreadReporter.MaybeReport();
        }
        if (probeRequested)
            frameProbeCapture.Complete();
        ++frameSwaps;
    }

    static uint16_t SwapIndex16(uint16_t v, uint32_t endian)
    {
        // xenos::Endian: k8in16 (1) swaps the two bytes of each 16-bit index.
        return endian == 1 ? uint16_t((v >> 8) | (v << 8)) : v;
    }

    void CaptureHotDraw(uint32_t opcode, const uint32_t *raw, uint32_t usable)
    {
        if (drawCaptureDone || !lucent::config::flag("DRAW_CAPTURE"))
            return;
        const bool anyShader = lucent::config::flag("DRAW_CAPTURE_ANY");
        if (!anyShader && g_shaderCapture.activeVertexHash != kHotVertexHash)
            return;

        // DRAW_INDX carries a viz-query token before VGT_DRAW_INITIATOR;
        // DRAW_INDX_2 does not (Xenia ExecutePacketType3_DRAW_INDX vs _2).
        const uint32_t initiatorIdx = (opcode == 0x22) ? 1u : 0u;
        if (usable <= initiatorIdx)
            return;
        const uint32_t initiator = raw[initiatorIdx];

        const uint32_t primType = initiator & 0x3F;
        const uint32_t sourceSelect = (initiator >> 6) & 0x3;
        const uint32_t majorMode = (initiator >> 8) & 0x3;
        const uint32_t indexSizeBit = (initiator >> 11) & 0x1; // 0=int16,1=int32
        const uint32_t numIndices = (initiator >> 16) & 0xFFFF;

        // A hot-pair draw with a populated fetch #95 is the target; gate the
        // one-shot on that so we do not latch on an early degenerate draw.
        const uint32_t fetchBase = kConstBaseFetch + kHotVertexFetchIndex * 2;
        const uint32_t vf0 = g_gpuRegisters[fetchBase];
        const uint32_t vf1 = g_gpuRegisters[fetchBase + 1];
        if (!anyShader && vf0 == 0)
            return;
        drawCaptureDone = true;

        namespace fs = std::filesystem;
        const char *dir = std::getenv("GEARS_DRAW_CAPTURE_DIR");
        fs::path outdir = dir ? fs::path(dir) : fs::path("scratch/draw-params");
        std::error_code ec;
        fs::create_directories(outdir, ec);
        std::ofstream rep(outdir / "hot_draw.txt");

        auto emit = [&](const std::string &s)
        {
            lucent::info("gpu", "{}", s);
            if (rep)
                rep << s << '\n';
        };

        static const char *kPrim[16] = {
            "none",           "point_list",   "line_list",      "line_strip",
            "triangle_list",  "triangle_fan", "triangle_strip", "triangle_w_wflags",
            "rectangle_list", "unused1",      "unused2",        "unused3",
            "line_loop",      "quad_list",    "quad_strip",     "polygon"};
        static const char *kSrc[4] = {"kDMA(indexed)", "kImmediate(inline)", "kAutoIndex",
                                      "invalid"};

        emit(std::format("=== hot-pair {} draw parameters ===", OpcodeName(opcode)));
        emit(std::format("bound shaders: vertex {:#018x} pixel {:#018x}",
                         g_shaderCapture.activeVertexHash, g_shaderCapture.activePixelHash));
        emit(std::format("VGT_DRAW_INITIATOR = {:#010x}", initiator));
        emit(std::format("  prim_type       = {:#x} ({})", primType,
                         primType < 16 ? kPrim[primType] : "explicit/other"));
        emit(std::format("  source_select   = {} ({})", sourceSelect, kSrc[sourceSelect]));
        emit(std::format("  major_mode      = {}", majorMode));
        emit(std::format("  index_size      = {} ({})", indexSizeBit,
                         indexSizeBit ? "int32" : "int16"));
        emit(std::format("  num_indices     = {}", numIndices));

        uint32_t dmaBase = 0, dmaSize = 0, dmaNumWords = 0, dmaSwap = 0;
        const uint32_t indexSizeBytes = indexSizeBit ? 4u : 2u;
        uint32_t indexGuestBase = 0, indexLenBytes = 0;
        if (sourceSelect == 0) // kDMA
        {
            const uint32_t baseIdx = initiatorIdx + 1;
            const uint32_t sizeIdx = initiatorIdx + 2;
            if (usable > sizeIdx)
            {
                dmaBase = raw[baseIdx];
                dmaSize = raw[sizeIdx];
                dmaNumWords = dmaSize & 0xFFFFFF;
                dmaSwap = (dmaSize >> 30) & 0x3;
                indexGuestBase = dmaBase & ~(indexSizeBytes - 1);
                indexLenBytes = dmaNumWords * indexSizeBytes;
            }
            emit(std::format("VGT_DMA_BASE = {:#010x}  VGT_DMA_SIZE = {:#010x}"
                             " (num_words {}, swap_mode {})",
                             dmaBase, dmaSize, dmaNumWords, dmaSwap));
            emit(std::format("  index buffer: guest_base {:#x}, {} indices,"
                             " {}-bit, {} bytes, endian {}",
                             indexGuestBase, numIndices, indexSizeBytes * 8, indexLenBytes,
                             dmaSwap));
        }
        else if (sourceSelect == 2)
        {
            emit("index source: kAutoIndex (no index buffer; indices 0..num_indices-1)");
        }

        // Geometry source: the captured draw profile uses fetch constant #95.
        const uint32_t vfType = vf0 & 0x3;
        const uint32_t vertexBaseBytes = (vf0 >> 2) << 2; // address<<2, byte addr
        const uint32_t vfEndian = vf1 & 0x3;
        const uint32_t vfSizeWords = (vf1 >> 2) & 0xFFFFFF;
        const uint32_t strideBytes = kHotVertexStrideDwords * 4;
        emit(std::format("vertex fetch constant #95 (reg {:#x}): {:#010x} {:#010x}", fetchBase, vf0,
                         vf1));
        emit(std::format("  type {} vertex_base {:#x} (dword addr {:#x}) stride {} dwords"
                         " ({} bytes) endian {} size {} words ({} bytes)",
                         vfType, vertexBaseBytes, vf0 >> 2, kHotVertexStrideDwords, strideBytes,
                         vfEndian, vfSizeWords, vfSizeWords * 4));

        // First N indices from guest memory.
        const uint32_t nIdx = std::min(numIndices, 32u);
        std::vector<uint32_t> indices;
        if (sourceSelect == 0 && indexGuestBase)
        {
            lucent::Line line;
            line.add("first {} indices:", nIdx);
            uint32_t minI = 0xFFFFFFFF, maxI = 0;
            for (uint32_t i = 0; i < numIndices; ++i)
            {
                uint32_t idx;
                if (indexSizeBytes == 2)
                {
                    // Read the physical bytes as a host-native u16 (== Xenia's
                    // load<uint16_t> from its physical buffer), then apply the
                    // GpuSwap the swap_mode selects (k8in16 == byte swap), exactly
                    // as Xenia's xenos::GpuSwap(uint16_t, endian) does.
                    const uint16_t *p = gears::Memory().Translate<uint16_t>(indexGuestBase + i * 2);
                    idx = SwapIndex16(*p, dmaSwap);
                }
                else
                    idx = ReadGuest32(indexGuestBase + i * 4);
                if (i < nIdx)
                {
                    indices.push_back(idx);
                    line.add(" {}", idx);
                }
                minI = std::min(minI, idx);
                maxI = std::max(maxI, idx);
            }
            line.flush(lucent::Level::Info, "gpu");
            if (rep)
            {
                rep << "first " << nIdx << " indices:";
                for (uint32_t v : indices)
                    rep << ' ' << v;
                rep << "\n";
            }
            emit(std::format("  index range: min {} max {} (vertex-buffer size {} bytes"
                             " admits index < {})",
                             minI, maxI, vfSizeWords * 4,
                             strideBytes ? vfSizeWords * 4 / strideBytes : 0));
        }

        // First few vertices from the shared-memory source. The captured stream
        // descriptor exposes a four-component float position at byte offset 0.
        const uint32_t nVtx = 6;
        emit(std::format("first {} vertices (dwords 0..3 = FMT_32_32_32_32_FLOAT"
                         " position attribute):",
                         nVtx));
        // Which vertices to sample: the first few referenced indices, or 0..n.
        std::vector<uint32_t> sampleVerts;
        if (!indices.empty())
            for (uint32_t i = 0; i < nVtx && i < indices.size(); ++i)
                sampleVerts.push_back(indices[i]);
        else
            for (uint32_t i = 0; i < nVtx; ++i)
                sampleVerts.push_back(i);

        for (uint32_t vi : sampleVerts)
        {
            const uint32_t vaddr = vertexBaseBytes + vi * strideBytes;
            uint32_t d[12];
            for (uint32_t k = 0; k < 12; ++k)
            {
                const uint32_t w = ReadGuest32(vaddr + k * 4); // k8in32 == full swap
                // vf endian: 2 (k8in32) matches ReadGuest32's full byteswap.
                d[k] = (vfEndian == 2)
                           ? w
                           : gears::LoadGpuWord32(gears::Memory().Translate<uint8_t>(vaddr + k * 4),
                                                  vfEndian);
            }
            float pos[4];
            for (int k = 0; k < 4; ++k)
                std::memcpy(&pos[k], &d[k], 4);
            emit(std::format("  v[{}] @ {:#x}: pos ({}, {}, {}, {})  raw"
                             " {:#010x} {:#010x} {:#010x} {:#010x}",
                             vi, vaddr, pos[0], pos[1], pos[2], pos[3], d[0], d[1], d[2], d[3]));
        }
        emit(std::format("(wrote to {})", (outdir / "hot_draw.txt").string()));
    }

    void HandleType3(uint32_t opcode, const uint32_t *data, uint32_t count, int depth)
    {
        ++(depth == 0 ? ringOpcodes : innerOpcodes)[opcode];
        // The bin registers are the tiling controls: on a predicated-tiling
        // replay the driver sets BIN_MASK/BIN_SELECT before each pass over the
        // same recorded buffer. Report every distinct value once so the tile
        // count (if any) is observable rather than assumed.
        if (opcode >= 0x50 && opcode <= 0x51)
            lucent::debug("gpu", "{} data {:#x} {:#x} (depth {})", OpcodeName(opcode),
                          count >= 1 ? data[0] : 0u, count >= 2 ? data[1] : 0u, depth);
        if (opcode >= 0x60 && opcode <= 0x63)
            lucent::debug("gpu", "{} data {:#x} (depth {})", OpcodeName(opcode),
                          count >= 1 ? data[0] : 0u, depth);

        switch (opcode)
        {
        // Bin mask/select maintenance. These are PFP registers, 64 bits each,
        // written either as a pair (0x50/0x51, hi word first) or half at a time
        // (0x60..0x63). They are the predication state everything else is
        // tested against.
        case 0x50:
            if (count >= 2)
                binMask = (uint64_t(data[0]) << 32) | data[1];
            break;
        case 0x51:
            if (count >= 2)
                binSelect = (uint64_t(data[0]) << 32) | data[1];
            break;
        case 0x60:
            if (count >= 1)
                binMask = (binMask & 0xFFFFFFFF00000000ull) | data[0];
            break;
        case 0x61:
            if (count >= 1)
                binMask = (binMask & 0xFFFFFFFFull) | (uint64_t(data[0]) << 32);
            break;
        case 0x62:
            if (count >= 1)
                binSelect = (binSelect & 0xFFFFFFFF00000000ull) | data[0];
            break;
        case 0x63:
            if (count >= 1)
                binSelect = (binSelect & 0xFFFFFFFFull) | (uint64_t(data[0]) << 32);
            break;

        case kOpIndirectBuffer:
        case kOpIndirectBufferPfd:
            if (count >= 2)
            {
                const uint32_t address = data[0] & ~3u;
                const uint32_t words = data[1] & 0xFFFFF;
                if (depth < 8 && address != 0 && words != 0)
                {
                    // Ring provenance matters here: the same indirect buffer
                    // being executed many times per frame is either the ring
                    // consumer re-reading one ring slot or the title genuinely
                    // re-submitting it, and only the ring dword index tells
                    // those apart.
                    if (depth == 0)
                    {
                        ++ibCounts[address];
                        lucent::debug("gpu", "IB {:#x} ({} words) from ring dword {:#x}", address,
                                      words, sourceIndex);
                    }
                    ExecuteLinear(address, words, depth + 1);
                }
            }
            break;

        case kOpWaitRegMem:
            if (count >= 5)
                WaitRegMem(data);
            break;

        case kOpMemWrite:
            for (uint32_t i = 1; i < count; i++)
                gears::StoreGpuPacketWord(data[0] + (i - 1) * 4, data[i]);
            break;

        case kOpEventWriteShd:
            // Hardware defers this write until prior GPU work retires; it does
            // not block the command processor while that work is outstanding.
            // Keep consuming packets and publish only the write at the render
            // thread's completion boundary. Blocking here serialized the guest
            // behind 50-200 ms live frames and collapsed gameplay to 5 fps.
            if (count >= 3)
                gears::DeferGpuRetirementWrite(data[1], data[2], sourceBase, sourceIndex);
            break;

        case kOpRuntimeSwap:
            // Frame boundary written by VdSwap. data[0] is the front buffer
            // address, data[1] a sequence number stamped at VdSwap time. The
            // sequence exists because the packet lives in guest command-buffer
            // memory that persists: a later submission reusing the buffer
            // without calling VdSwap re-presents the stale packet (measured:
            // CP swap executions far outnumbered guest VdSwap calls). A stale
            // copy carries an old sequence and is skipped.
            if (count >= 2)
            {
                if (int32_t(data[1] - lastSwapSequence) > 0)
                {
                    lastSwapSequence = data[1];
                    lucent::debug("gpu", "swap packet: front buffer {:#x} (seq {})", data[0],
                                  data[1]);
                    ReportWaitStats();
                    gears::ReportDrawPacketWriteWatch();
                    // Whenever the ramp changes, say what it now is. Once per
                    // swap at most, and only on a change, so a title that
                    // uploads once says it once.
                    {
                        const uint32_t writes = g_gammaRamp.directWrites + g_gammaRamp.seqWrites +
                                                g_gammaRamp.pwlWrites;
                        if (writes != g_gammaRamp.reportedWrites)
                        {
                            g_gammaRamp.reportedWrites = writes;
                            uint32_t differing = 0;
                            lucent::Line diff;
                            for (uint32_t i = 0; i < 256; ++i)
                            {
                                const uint32_t v = i * 0x3FF / 0xFF;
                                const uint32_t linear = v | (v << 10) | (v << 20);
                                if (g_gammaRamp.table[i] == linear)
                                    continue;
                                ++differing;
                                // The entries themselves, not just a count: "one
                                // entry differs" is compatible with a ramp that
                                // reshapes the image and with a rounding artefact
                                // of the linear table this compares against, and
                                // those call for opposite conclusions.
                                if (differing <= 4)
                                    diff.add(" [{}] {:#010x} vs linear {:#010x}", i,
                                             g_gammaRamp.table[i], linear);
                            }
                            if (differing)
                                diff.flush(lucent::Level::Info, "gpu");
                            lucent::info("gpu",
                                         "gamma ramp CHANGED: {} write(s)"
                                         " ({} whole-entry DC_LUT_30_COLOR, {} per-channel"
                                         " DC_LUT_SEQ_COLOR, {} PWL); {} of 256 entries now"
                                         " differ from linear, so scan-out is NOT the"
                                         " identity and a frame presented without it is"
                                         " brighter than the console's",
                                         writes, g_gammaRamp.directWrites, g_gammaRamp.seqWrites,
                                         g_gammaRamp.pwlWrites, differing);
                        }
                    }
                    // Whole-frame guest-draw backend: at the first swap that has
                    // accumulated draws, render them all into a persistent target.
                    TriggerCpStall();
                    // data[0] is the front buffer this swap presents. The renderer
                    // needs it BEFORE it chooses what to present.
                    SetFrontBuffer(data[0]);
                    // Said once, at the first swap that has draws behind it:
                    // whether this title uploads a gamma ramp at all, and by
                    // which path. A silent renderer cannot distinguish "no ramp
                    // was programmed" from "we never looked", and those call for
                    // opposite work.
                    if (!g_gammaRamp.reported)
                    {
                        g_gammaRamp.reported = true;
                        const uint32_t writes = g_gammaRamp.directWrites + g_gammaRamp.seqWrites +
                                                g_gammaRamp.pwlWrites;
                        if (!writes)
                        {
                            // Not the end of the story, and the line says so:
                            // this title programs its ramp long after boot, so a
                            // first-frame-only report would answer the wrong
                            // question and read as a settled negative.
                            (void)0;
                            lucent::info("gpu", "gamma ramp: NONE programmed by"
                                                " the first presented frame. This title uploads one"
                                                " later, so this is a starting state, not a"
                                                " conclusion -- watch for the follow-up line");
                        }
                        else
                        {
                            // Non-identity is the fact that matters: a ramp equal
                            // to the linear default changes no pixel, so uploading
                            // one is not by itself evidence of anything.
                            uint32_t differing = 0;
                            for (uint32_t i = 0; i < 256; ++i)
                            {
                                const uint32_t v = i * 0x3FF / 0xFF;
                                if (g_gammaRamp.table[i] != (v | (v << 10) | (v << 20)))
                                    ++differing;
                            }
                            lucent::info("gpu",
                                         "gamma ramp: {} entry write(s)"
                                         " ({} whole-entry via DC_LUT_30_COLOR, {} per-channel"
                                         " via DC_LUT_SEQ_COLOR, {} PWL), and {} of 256 entries"
                                         " differ from linear. Xenia implements only the"
                                         " SEQ_COLOR path, so a ramp uploaded the other way"
                                         " is one the reference is NOT applying either",
                                         writes, g_gammaRamp.directWrites, g_gammaRamp.seqWrites,
                                         g_gammaRamp.pwlWrites, differing);
                        }
                    }
                    // data[2..7] is the front buffer's fetch constant, written by
                    // VdSwap. A packet from before that was added carries zeros
                    // there, and zeros are recorded as such rather than as a
                    // fetch constant describing address 0.
                    if (count >= 8)
                        SetFrontBufferFetch(&data[2]);
                    TriggerFrameRender();
                    // The frame boundary is here, at the point in the stream
                    // where the hardware would flip -- so this is where the
                    // host swapchain is presented. Stale copies of the packet
                    // are already filtered above, so this is one present per
                    // guest VdSwap. The host backend lives in gpu_present.cpp;
                    // it must not accrete into the command processor.
                    gears::PresentFrame(data[0], data[1]);
                }
                else
                {
                    lucent::debug("gpu", "stale swap packet ignored (seq {} <= {})", data[1],
                                  lastSwapSequence);
                }
            }
            break;

        case kOpEventWriteZpd:
            // Z-pass-done event: the GPU writes an xe_gpu_depth_sample_counts
            // record (0x20 bytes: Total/ZFail/ZPass/StencilFail A+B pairs,
            // little-endian) at the record the address in RB_SAMPLE_COUNT_ADDR
            // selects. D3D pre-fills records with the 0xFFFFFEED sentinel and
            // its occlusion-query path polls until the event overwrites it --
            // the post-load no-present spin was this poll against a record no
            // one wrote. A GPU that rasterises
            // nothing has zero samples in every counter, so the record is
            // zero-filled, which also clears the sentinel. Layout and
            // addressing per Xenia's xenos_zpd_report.h (record = addr &
            // ~0x1F; END at slot+0, BEGIN at slot+0x20).
            //
            // ZERO IS NOT A NEUTRAL ANSWER, IT IS "NOTHING WAS VISIBLE". D3D
            // computes the query result as END.ZPass - BEGIN.ZPass (A and B
            // summed), so zero-filling every record answers every occlusion
            // query with "no pixels passed" -- and the title BELIEVES it. That
            // was harmless when this was written, because the renderer really
            // did rasterise nothing; it stopped being harmless the moment the
            // renderer started drawing, and nothing brought the comment or the
            // code back.
            //
            // So the default answers "everything was visible", by making the
            // counter MONOTONICALLY INCREASING so that any END minus any
            // earlier BEGIN is positive. Conservative in the only direction
            // that is safe: over-reporting visibility costs drawing something
            // that was hidden, while under-reporting DELETES geometry the title
            // asked for. Measured on the title screen -- the post-processing
            // pass group went from 2.0% of frames to 100.0%, draws per frame
            // from a median of 161 to 171 against the console's 173, and the
            // screen from grey-brown to red (R/G 1.79 -> 2.68, console 3.52).
            //
            // GEARS_GPU_ZPD_ZERO=1 restores the old all-zero answer. A
            // DIAGNOSTIC control arm for exactly this measurement, never a fix.
            //
            // STOPGAP: the proper fix is real occlusion queries -- a Vulkan
            // OCCLUSION query pool around the draws between the BEGIN and END
            // events, resolved back into this record -- because this reports a
            // fixed answer rather than measuring one. It is a stopgap and not
            // the fix, but it is a stopgap over a WRONG answer rather than
            // over a missing one.
            {
                const uint32_t reportAddress = g_gpuRegisters[kRegSampleCountAddr];
                const uint32_t recordBase = reportAddress & ~0x1Fu;
                if (recordBase != 0)
                {
                    uint8_t *record = gears::Memory().Translate<uint8_t>(recordBase);
                    // The zero-fill stays: it is what clears D3D's 0xFFFFFEED
                    // sentinel, which is how GetData learns the query is done.
                    memset(record, 0, 0x20);
                    static const bool reportVisible = !lucent::config::flag("GPU_ZPD_ZERO");
                    if (reportVisible)
                    {
                        // Per query, not per pixel: the value only has to grow
                        // between the BEGIN record and the END record that
                        // follows it. Guest-side little-endian, which is the
                        // host's own order here, so a plain store is correct.
                        static std::atomic<uint32_t> zpassCounter{0};
                        const uint32_t samples =
                            zpassCounter.fetch_add(kZpdSamplesPerQuery, std::memory_order_relaxed) +
                            kZpdSamplesPerQuery;
                        std::memcpy(record + offsetof(GuestDepthSampleCounts, zpassA), &samples, 4);
                        std::memcpy(record + offsetof(GuestDepthSampleCounts, total_A), &samples,
                                    4);
                    }
                    lucent::debug("gpu",
                                  "EVENT_WRITE_ZPD: {} -> {:#x}"
                                  " (initiator {:#x})",
                                  reportVisible ? "visible (monotonic ZPass)" : "zero samples",
                                  recordBase, count >= 1 ? data[0] : 0u);
                }
            }
            break;

        case kOpEventWriteExt:
            // Screen-extent event: writes six 16-bit values (min/max x, y, z
            // of pixels affected by the previous draw) 8-in-16 swapped. With
            // nothing rasterised the truthful extent is empty, but the
            // consumer computes tile bounds from it and an inverted empty box
            // is a shape hardware never produces; the full-surface extent is
            // the conservative answer a tiling optimiser must always accept.
            // Values and byte order follow Xenia's EVENT_WRITE_EXT handler.
            if (count >= 2)
            {
                const uint32_t address = data[1] & ~3u;
                const uint16_t extents[6] = {
                    0,         // min x (in 8px blocks)
                    8192 >> 3, // max x
                    0,         // min y
                    8192 >> 3, // max y
                    0,         // min z
                    1,         // max z
                };
                auto *out = gears::Memory().Translate<uint16_t>(address);
                for (int i = 0; i < 6; i++)
                    out[i] = uint16_t(extents[i] << 8 | extents[i] >> 8);
                lucent::debug("gpu", "EVENT_WRITE_EXT -> {:#x}", address);
            }
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
                        const auto isrStart = std::chrono::steady_clock::now();
                        interruptState.Dispatch(1, cpu);
                        interruptUs +=
                            uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - isrStart)
                                         .count());
                        ++interrupts;
                    }
                }
            }
            break;

        default:
            break; // no draw hardware behind this; skipped by count
        }
    }

    void WaitRegMem(const uint32_t *data)
    {
        const uint32_t waitInfo = data[0];
        const uint32_t poll = data[1];
        const uint32_t ref = data[2];
        const uint32_t mask = data[3];

        const auto start = std::chrono::steady_clock::now();
        bool reported = false;
        auto &stat = waitStats[(waitInfo & 0x10) ? (poll & ~3u) : (poll & 0x7FFF)];
        ++stat.first;
        const auto accumulate = [&]
        {
            const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
            stat.second += us;
            regWaitUs += us;
        };
        for (;;)
        {
            const uint32_t raw =
                (waitInfo & 0x10) ? gears::LoadGpuPacketWord(poll) : g_gpuRegisters[poll & 0x7FFF];
            const uint32_t value = raw & mask;

            bool matched;
            switch (waitInfo & 7)
            {
            case 1:
                matched = value < ref;
                break;
            case 2:
                matched = value <= ref;
                break;
            case 3:
                matched = value == ref;
                break;
            case 4:
                matched = value != ref;
                break;
            case 5:
                matched = value >= ref;
                break;
            case 6:
                matched = value > ref;
                break;
            default:
                matched = true;
                break;
            }
            if (matched)
            {
                accumulate();
                return;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(50));
            if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(5))
            {
                reported = true;
                lucent::error("gpu",
                              "WAIT_REG_MEM stuck: info={:#x} poll={:#x} ref={:#x}"
                              " mask={:#x} value={:#x}",
                              waitInfo, poll, ref, mask, value);
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
            i += ExecutePacket(
                header, [&](uint32_t w) { return ReadGuest32(base + (i + w) * 4); }, words - i,
                depth);
        }
    }

    // Executes one packet whose header has been consumed; `fetch(w)` reads
    // data word w. Returns the number of data words consumed.
    template <typename Fetch>
    uint32_t ExecutePacket(uint32_t header, Fetch &&fetch, uint32_t available, int depth)
    {
        const uint32_t type = header >> 30;
        if (header == 0 || type == 2)
            return 0;

        const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        const uint32_t usable = std::min(count, available);

        if (type == 0)
        {
            const uint32_t baseRegister = header & 0x7FFF;
            gears::GpuRegisterWriteScope src(
                !gears::GpuRegisterWatchEnabled()
                    ? std::string()
                    : std::format("TYPE0 base {:#x} x{}{}", baseRegister, usable,
                                  (header & 0x8000) ? " one-reg" : ""));
            const bool oneRegister = (header & 0x8000) != 0;
            for (uint32_t w = 0; w < usable; w++)
            {
                const uint32_t reg = oneRegister ? baseRegister : baseRegister + w;
                // Census the direct-register path into the constant files: the
                // stream also programs the ALU/fetch files as plain TYPE0 writes,
                // so this counts them separately from the SET_CONSTANT path.
                if (reg >= kConstBaseAlu && reg < kConstBaseFetch)
                    ++type0AluWrites;
                else if (reg >= kConstBaseFetch && reg < kConstBaseBool)
                    ++type0FetchWrites;
                WriteGpuRegister(reg, fetch(w));
            }
            // A watched register that receives something that cannot be a float
            // -- a guest pointer -- is either a real upload of a real pointer or
            // a stream we are walking out of step. Those two look identical from
            // the register write alone, so dump the WHOLE payload the first few
            // times it happens: a genuine constant block reads as floats around
            // the pointer, a misparse reads as command words. Capped by novelty
            // (first three), and the cap is stated so a silent stop is not read
            // as "it stopped happening".
            if (gears::GpuRegisterWatchEnabled() && !oneRegister)
            {
                static uint32_t dumped = 0;
                for (uint32_t reg : gears::GpuRegisterWatchRegisters())
                {
                    if (reg < baseRegister || reg - baseRegister >= usable)
                        continue;
                    const uint32_t v = fetch(reg - baseRegister);
                    if ((v & 0xFF000000u) != 0xA0000000u || dumped >= 3)
                        continue;
                    ++dumped;
                    lucent::Line line;
                    line.add("GPU_REG_WATCH payload dump {}/3: header {:08x} (TYPE0 base"
                             " {:#x} x{}) at {:#x}+{}, watched {:#x} got {:#010x} at word"
                             " {}; payload then 8 words past it:",
                             dumped, header, baseRegister, usable, sourceBase, sourceIndex, reg, v,
                             reg - baseRegister);
                    for (uint32_t w = 0; w < usable; ++w)
                        line.add(" {:08x}", fetch(w));
                    line.add(" |");
                    for (uint32_t w = usable; w < usable + 8; ++w)
                        line.add(" {:08x}", fetch(w));
                    // A dropped dereference -- the recompiler emitting the
                    // ADDRESS of a float where the guest loads the float --
                    // would leave the wanted value sitting AT the address, so
                    // read it. If these words are not a plausible float the
                    // pointer means something else, which is equally an answer.
                    line.add(" | at {:#010x}:", v);
                    for (uint32_t w = 0; w < 8; ++w)
                        line.add(" {:08x}", ReadGuest32((v & 0x1FFFFFFFu) + w * 4));
                    line.flush(lucent::Level::Info, "gpu");
                }
            }
            return count;
        }
        if (type == 1)
        {
            gears::GpuRegisterWriteScope src("TYPE1");
            if (usable >= 1)
                WriteGpuRegister(header & 0x7FF, fetch(0));
            if (usable >= 2)
                WriteGpuRegister((header >> 11) & 0x7FF, fetch(1));
            return count;
        }

        // TYPE3. The handled opcodes carry at most 18 words; larger packets
        // are state uploads, skipped without copying.
        const uint32_t opcode = (header >> 8) & 0x7F;
        gears::GpuRegisterWriteScope src(!gears::GpuRegisterWatchEnabled() ? std::string()
                                                                           : OpcodeName(opcode));
        uint32_t data[20];
        const uint32_t copy = std::min<uint32_t>(usable, 20);
        for (uint32_t w = 0; w < copy; w++)
            data[w] = fetch(w);

        // Predication: bit 0 of the header marks a packet the PFP executes only
        // while the bin registers still select a live bin. The title records one
        // command buffer and replays it per EDRAM tile, masking off the packets
        // that do not belong to the tile being rendered; a consumer that ignores
        // the predicate executes every packet on every pass, including the
        // INTERRUPT that re-enqueues the command list.
        const bool anyPass = (binSelect & binMask) != 0;
        if (!anyPass)
            ++predicateOffPackets;
        if (header & 1)
        {
            ++predicatedSeen[opcode];
            if (!anyPass)
            {
                ++predicatedSkip[opcode];
                return count;
            }
        }

        // Shader loads are handled here rather than in HandleType3 because
        // IM_LOAD_IMMEDIATE carries its whole microcode payload inline, which
        // does not fit the 20-word copy above.
        if (opcode == kOpImLoad || opcode == kOpImLoadImmediate)
            CaptureShaderLoad(opcode, fetch, usable, count);

        // Constant-file loads are likewise handled from fetch(): a SET_CONSTANT
        // can carry the whole 1024-dword ALU file, far past the 20-word copy.
        // This populates the register file the translated shaders read as UBOs.
        if (opcode == kOpSetConstant || opcode == kOpLoadAluConstant || opcode == kOpSetConstant2 ||
            opcode == kOpSetShaderConstants)
            TrackConstantLoad(opcode, fetch, usable, count);

        // Verification hook: dump the constant files at the first real draw.
        if (opcode == 0x22 || opcode == 0x36) // DRAW_INDX / DRAW_INDX_2
        {
            gears::MaybeArmDrawPacketWriteWatch(sourceBase, sourceIndex, depth, lastSwapSequence);

            // Mirror VGT_DRAW_INITIATOR from the packet into the register file so
            // prim_type / index_size are live for the system-constants
            // derivation (DRAW_INDX carries a viz token before the initiator,
            // DRAW_INDX_2 does not). This is the seam fix: the value is tracked,
            // not injected by flag.
            const uint32_t initiatorIdx = (opcode == 0x22) ? 1u : 0u;
            const uint32_t initiator = copy > initiatorIdx ? data[initiatorIdx] : 0u;
            if (copy > initiatorIdx)
                WriteGpuRegister(kRegDrawInitiator, initiator);

            if (lucent::config::flag("DRAW_CENSUS"))
            {
                ++drawsSeen;
                const auto key = std::make_pair(g_shaderCapture.activeVertexHash,
                                                g_shaderCapture.activePixelHash);
                ++drawPairs[key];
                lucent::info("draw",
                             "draw #{} {} prim {:#x} src {} idx32 {} indices {}"
                             " vs {:#018x} ps {:#018x}",
                             drawsSeen, OpcodeName(opcode), initiator & 0x3F,
                             (initiator >> 6) & 0x3, (initiator >> 11) & 0x1,
                             (initiator >> 16) & 0xFFFF, g_shaderCapture.activeVertexHash,
                             g_shaderCapture.activePixelHash);
            }

            DumpConstantFiles(opcode);
            CaptureHotDraw(opcode, data, copy);
            CaptureFrameDraw(opcode, data, copy, initiator);
        }

        HandleType3(opcode, data, copy, depth);
        return count;
    }

    // The ring consumer. rptr/wptr are dword indices; the write pointer is
    // whatever the title last stored to CP_RB_WPTR through the device window.
    void Run()
    {
        ShaderCaptureInit();
        if (!interruptState.Init())
        {
            lucent::error("gpu", "cannot create the command processor's guest block");
            return;
        }

        lucent::info("gpu", "command processor consuming ring {:#x} ({} bytes)", g_ringBuffer.base,
                     g_ringBuffer.Bytes());

        constexpr uint32_t kCpRbWptr = 0x7FC80714;
        const uint32_t dwords = g_ringBuffer.Dwords();
        uint32_t rptr = 0;
        uint32_t lastWptr = 0;
        StoreGuest32(g_ringBuffer.readPtrWriteBackAddress, rptr);

        for (;;)
        {
            const uint32_t wptr = ReadGuest32(kCpRbWptr) & (dwords - 1);
            if (wptr != lastWptr)
            {
                const uint32_t advance = (wptr - lastWptr) & (dwords - 1);
                wptrTotal += advance;
                frameWptrAdvance += advance;
                lastWptr = wptr;
            }
            if (wptr == rptr)
            {
                const auto idleStart = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                idleUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - idleStart)
                                       .count());
                ++idlePolls;
                continue;
            }

            // Everything the title has actually written and we have not yet
            // consumed. A ring consumer must never read past this: beyond the
            // write pointer the memory holds whatever the previous lap left.
            const uint32_t available = (wptr - rptr) & (dwords - 1);

            const uint32_t header = ReadGuest32(g_ringBuffer.base + rptr * 4);
            sourceBase = 0;
            sourceIndex = rptr;
            // Kept separately: sourceIndex is a member and nested indirect
            // buffer execution overwrites it, so anything logged after the
            // packet runs would report the last packet inside the IB instead.
            const uint32_t ringIndex = rptr;
            rptr = (rptr + 1) & (dwords - 1);
            const uint32_t consumed = ExecutePacket(
                header, [&](uint32_t w)
                { return ReadGuest32(g_ringBuffer.base + ((rptr + w) & (dwords - 1)) * 4); },
                dwords, 0);

            if (consumed + 1 > available)
            {
                // The packet claims to extend past what has been written, so a
                // length was misread. Advancing by it would carry rptr beyond
                // wptr, and since the comparison below is an equality test the
                // consumer would then lap the whole ring, re-executing every
                // stale packet on it -- old swap packets and old fences
                // included. That is not a hypothetical: it is what left the
                // title submitting nothing while the ring appeared busy.
                //
                // Reported at error level with the header, because clamping
                // hides the misparse that caused it and the misparse is the
                // real defect.
                lucent::error("gpu",
                              "packet at ring dword {:#x} claims {} dwords with only"
                              " {} written (header {:#010x}); resynchronising to the write pointer",
                              sourceIndex, consumed + 1, available, header);
                rptr = wptr;
            }
            else
            {
                rptr = (rptr + consumed) & (dwords - 1);
            }

            // Raw ring-level trace: dword index, header, dwords the parser
            // charged for it. The consumer must account for every dword the
            // title wrote, so any shortfall shows up as a gap between one
            // packet's index+length and the next packet's index.
            lucent::debug("ring", "{:#06x} {:#010x} +{} wptr {:#06x}->{:#06x}", ringIndex, header,
                          consumed + 1, wptr, ReadGuest32(kCpRbWptr) & (dwords - 1));

            rptrTotal += consumed + 1;
            frameRptrAdvance += consumed + 1;
            if (!overshootReported && rptrTotal > wptrTotal)
            {
                overshootReported = true;
                lucent::error("gpu",
                              "ring overshoot: consumed {} dwords but only {} written;"
                              " packet at dword {:#x} header {:#010x} claimed {} dwords",
                              rptrTotal, wptrTotal, sourceIndex, header, consumed + 1);
            }

            StoreGuest32(g_ringBuffer.readPtrWriteBackAddress, rptr);
        }
    }
};

void CommandProcessorThread()
{
    // Brought up here rather than in main(): the presenter only has anything to
    // do once there is a command stream, and this keeps the whole graphics
    // backend off the guest's own threads. A false return means no display --
    // a supported outcome, and the command processor is unchanged by it.
    const bool haveWindow = gears::PresenterStart();
    // The pad's source depends on whether a window came up, so input is brought
    // up once that is known.
    gears::InitialiseInput(haveWindow);
    gears::StartDebugHttpServer();

    CommandProcessor cp;
    cp.Run();
    gears::StopDebugHttpServer();
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

// GUEST FRAMES PRESENTED. The counter VdSwap advances, exposed so the input
// script can be driven by it instead of by the wall clock: two emulators of one
// title run at different speeds, so a schedule keyed to the clock reaches a
// different point in the game on each. Keyed to this, "frame 1500" is the same
// game moment on both sides. The oracle exposes the same thing
// (CommandProcessor::guest_swap_count) and counts the same event.
uint64_t GuestFramesPresented()
{
    return g_frameCount.load(std::memory_order_relaxed);
}

// Registered at static init: the pointer is all input.cpp needs, and handing it
// over here keeps the dependency one-way (GPU -> input), so the kernel tests
// still link input.cpp without this file.
const bool g_frameSourceRegistered = []
{
    gears::SetGuestFrameSource(&GuestFramesPresented);
    return true;
}();

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
bool CommitDeviceWindow(GuestMemory &memory)
{
    constexpr uint32_t kDeviceWindowBase = 0x7FC00000;
    constexpr uint32_t kDeviceWindowSize = 0x00400000;

    if (!memory.Commit(kDeviceWindowBase, kDeviceWindowSize))
        return false;

    // One register in this window is not inert, because leaving it zero is not
    // a neutral choice.
    //
    // Bit 0 of the vblank status register gates the observed vblank interrupt
    // path. Guest code samples but never writes it, so the GPU owns the value.
    //
    // The runtime is the GPU here and it does deliver vblank, at 60 Hz from a
    // host thread. Reporting the bit clear while delivering the interrupt
    // describes a machine that cannot exist. It stays set rather than
    // being latched and cleared because the title never acknowledges it.
    //
    // The guest observes this word in guest byte order. Host byte order leaves
    // bit 0 clear from the guest's perspective and suppresses vblank handling.
    constexpr uint32_t kVblankStatusRegister = 0x7FC86544;
    *memory.Translate<uint32_t>(kVblankStatusRegister) = ByteSwap(1u);

    // Frame pacing observes scanline progress relative to the vertical total.
    // A zero total is invalid; the reported 1280x720p60 mode uses 750 lines
    // (CEA-861). A zero scanline models the display in vblank and agrees with
    // the status bit above. The guest observes both words in guest byte order.
    constexpr uint32_t kVerticalTotalRegister = 0x7FC86584;
    *memory.Translate<uint32_t>(kVerticalTotalRegister) = ByteSwap(750u);

    lucent::info("gpu", "device MMIO window {:#x}..{:#x} committed; vblank status set",
                 kDeviceWindowBase, kDeviceWindowBase + kDeviceWindowSize);
    return true;
}

} // namespace gears

void __imp__VdInitializeEngines(PPCContext &__restrict ctx, uint8_t *)
{
    if (const char *watch = getenv("GEARS_PM4_WATCH"))
    {
        g_pm4WatchAddress = uint32_t(strtoul(watch, nullptr, 16));
        lucent::info("gpu", "command stream will be traced for writes to {:#x}", g_pm4WatchAddress);
    }

    lucent::info("gpu", "VdInitializeEngines -- command processor up; the frame's"
                        " draws are rendered by the guest-draw backend at each swap");
    ctx.r3.u64 = 1;
}

void __imp__VdShutdownEngines(PPCContext &__restrict ctx, uint8_t *)
{
    lucent::info("gpu", "VdShutdownEngines after {} submitted frames", g_frameCount.load());
    ctx.r3.u64 = 0;
}

void __imp__VdInitializeRingBuffer(PPCContext &__restrict ctx, uint8_t *)
{
    g_ringBuffer.base = ctx.r3.u32;
    g_ringBuffer.sizeLog2 = ctx.r4.u32;
    lucent::info("gpu", "ring buffer at {:#x}, {} bytes", g_ringBuffer.base, g_ringBuffer.Bytes());
    StartCommandProcessor();
    ctx.r3.u64 = 0;
}

void __imp__VdEnableRingBufferRPtrWriteBack(PPCContext &__restrict ctx, uint8_t *)
{
    g_ringBuffer.readPtrWriteBackAddress = ctx.r3.u32;
    g_ringBuffer.readPtrWriteBackBlockSize = ctx.r4.u32;
    lucent::info("gpu", "ring buffer read-pointer write-back at {:#x}",
                 g_ringBuffer.readPtrWriteBackAddress);
    StartCommandProcessor();
    ctx.r3.u64 = 0;
}

void __imp__VdSetSystemCommandBufferGpuIdentifierAddress(PPCContext &__restrict ctx, uint8_t *)
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
    uint64_t vblanksDelivered = 0;
    uint64_t paceBase = 0;
    while (true)
    {
        // FREE-RUNNING VBLANK: no host sleep at all. `state.Dispatch` below runs
        // the title's ISR synchronously on this thread, so when it returns the
        // guest HAS consumed this vblank -- looping straight back is exactly
        // "deliver the next one as soon as the guest is ready for it", with no
        // host clock anywhere in the loop. That is what takes real time out of
        // the guest's notion of time; see runtime/guest_clock.h.
        const gears::ClockTrigger trigger = gears::GuestClockTrigger();
        if (trigger == gears::ClockTrigger::kVblankPaced)
        {
            // PHASE 1, before the first present: free-run, because the boot spin
            // waits on TIME and nothing else releases it.
            // PHASE 2, after it: deliver a vblank only while the vblank count is
            // within `slack` of the present count, so guest time advances at the
            // rate the guest actually renders at. Free-running instead is what
            // froze the picture -- guest time ran hundreds of times ahead of the
            // frames being drawn.
            //
            // `paceBase` is the vblank count at the first present, so phase 1's
            // consumption does not count against phase 2's budget. Without it
            // the gate would block until presents caught up with the whole boot,
            // which is a hang that looks exactly like the deadlock this mode
            // exists to avoid.
            const uint64_t presents = g_frameCount.load(std::memory_order_relaxed);
            if (presents == 0)
            {
                paceBase = vblanksDelivered;
            }
            else
            {
                const uint64_t budget = presents + gears::GuestClockVblankSlack();
                // yield, not sleep: waiting on the guest to present is waiting on
                // GUEST progress, and putting a host duration here would be the
                // real time this whole mode exists to keep out.
                // A GATE THAT BLOCKS FOREVER MUST SAY SO. Silence here is
                // indistinguishable from the boot deadlock this mode was built
                // to avoid, and telling the two apart by dump count alone is
                // what sent this design down a wrong diagnosis once already.
                uint64_t spins = 0;
                while (vblanksDelivered - paceBase >= budget &&
                       g_frameCount.load(std::memory_order_relaxed) == presents)
                {
                    if (++spins == 20000000ull)
                        lucent::warn("time",
                                     "vblank-paced is STARVED: {} vblanks"
                                     " delivered since the first present, budget {} ({}"
                                     " presents + {} slack), and the guest has not"
                                     " presented again. The title needs more vblanks per"
                                     " present than this gate allows -- which it"
                                     " legitimately does while loading, when it presents"
                                     " rarely and still expects time to pass",
                                     vblanksDelivered - paceBase, budget, presents,
                                     gears::GuestClockVblankSlack());
                    std::this_thread::yield();
                }
            }
        }
        else if (trigger != gears::ClockTrigger::kVblankFreeRun)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(16667));
        }
        ++vblanksDelivered;

        if (trigger != gears::ClockTrigger::kPresent)
            gears::AdvanceGuestClockFrame();

        // YIELD, BUT DO NOT SLEEP. Dispatch below holds g_interruptMutex for the
        // whole of the title's ISR, and looping straight back re-takes it with
        // no gap at all -- which starves every other path that needs it. Left
        // unyielded this mode presented 19, 3 and 0 frames on three identical
        // runs; the real clock presents 9 every time.
        //
        // A yield is host SCHEDULING, not host TIME: it changes which thread
        // runs next, exactly as the OS already does for every other guest
        // thread, and it puts nothing into the clock the guest reads. That is
        // the line this mode has to hold -- no host duration may enter guest
        // time -- and a yield does not cross it.
        // Every host-sleep-free mode needs this, not just free-run: without a
        // sleep the loop re-takes g_interruptMutex with no gap at all and
        // starves every other path that needs it. Measured -- vblank-paced
        // without the yield presented 0 frames on two runs, which reads exactly
        // like the boot deadlock and is not it.
        if (trigger == gears::ClockTrigger::kVblankFreeRun ||
            trigger == gears::ClockTrigger::kVblankPaced)
            std::this_thread::yield();

        // Sampled from here rather than from VdSwap: the title submits one
        // frame and then waits, so by the time it is stuck there are no more
        // swaps to hang a trace off, and the ring only has contents to read
        // after that first submission.
        if (g_pm4WatchAddress != 0 && g_ringBuffer.base != 0 && ++tick % 60 == 0)
        {
            gears::TraceCommandStream(g_ringBuffer.base, g_ringBuffer.Dwords(), g_pm4WatchAddress);
        }

        state.Dispatch(0, 0);
    }
}
} // namespace

void __imp__VdSetGraphicsInterruptCallback(PPCContext &__restrict ctx, uint8_t *)
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
void __imp__VdSwap(PPCContext &__restrict ctx, uint8_t *)
{
    const uint64_t frame = g_frameCount.fetch_add(1) + 1;
    // The guest's clock advances HERE only when the trigger is `present`.
    // EXACTLY ONE trigger drives it -- two would double the step and make the
    // guest's second read of the same frame's time disagree with its first.
    // See runtime/guest_clock.h for why `present` deadlocks this title.
    if (gears::GuestClockTrigger() == gears::ClockTrigger::kPresent)
        gears::AdvanceGuestClockFrame();
    // GEARS_WORK_TRACE=<path>: the guest's kernel-call count at every present.
    // Catalog #84's fourth candidate clock is "advance time per unit of GUEST
    // WORK", and it is only worth building if a measure of guest work is itself
    // reproducible. Two runs, same script, diff the files -- that is the whole
    // experiment, and it costs one atomic read per frame.
    {
        static const std::string &workTrace = lucent::config::text("WORK_TRACE");
        if (!workTrace.empty())
        {
            static std::FILE *f = std::fopen(workTrace.c_str(), "wb");
            if (f)
            {
                std::fprintf(f, "%llu %llu\n", (unsigned long long)frame,
                             (unsigned long long)gears::GuestKernelCalls());
                std::fflush(f);
            }
        }
    }
    gears::HleDumpCensus("swap");
    gears::HleWorkerCensus();
    gears::ReportRhiSemanticFrame(frame);
    // Drawing is progress even when the guest is making no kernel calls, so the
    // stall detector does not report a busy renderer as a dead guest.
    gears::NoteGuestProgress("draw");

    if (frame == 1 || frame % 60 == 0)
    {
        // Frame rate is the metric the presentation work is judged on, so
        // report it measured rather than leaving it to be inferred from log
        // line counts (the log has no timestamps).
        static std::chrono::steady_clock::time_point last;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = frame == 1 ? 0.0 : std::chrono::duration<double>(now - last).count();
        last = now;
        // Cheap, once per sixty frames, and it answers a question that cost six
        // runs: is the crash reporter still the one installed?
        gears::VerifyFaultReporterStillInstalled();
        lucent::info("gpu", "VdSwap: {} frames submitted, last 60 in {:.2f}s ({:.2f} fps)", frame,
                     seconds, seconds > 0 ? 60.0 / seconds : 0.0);
    }

    const uint32_t block = ctx.r3.u32;
    if (block != 0)
    {
        const uint32_t frontBuffer = ctx.r8.u32 != 0 ? ReadGuest32(ctx.r8.u32) : 0;

        StoreGuest32(block,
                     (3u << 30) | ((kSwapReservationDwords - 2) << 16) | (kOpRuntimeSwap << 8));
        StoreGuest32(block + 4, frontBuffer);
        StoreGuest32(block + 8, uint32_t(frame)); // sequence, see kOpRuntimeSwap
        // r4 is the front buffer's Direct3D 9 texture header fetch constant, six
        // dwords. The real kernel posts them to the sequencer as a TYPE0 write to
        // SHADER_CONSTANT_FETCH_00 immediately before the swap, because the
        // hardware takes the front buffer's format, size and tiling from fetch
        // slot 0 rather than from the address. Carrying them in the swap packet
        // keeps that statement of the guest's alongside the address it belongs
        // to, and behind the same stale-sequence filter.
        for (uint32_t i = 0; i < 6; i++)
            StoreGuest32(block + 12 + i * 4, ctx.r4.u32 != 0 ? ReadGuest32(ctx.r4.u32 + i * 4) : 0);
        for (uint32_t i = 9; i < kSwapReservationDwords; i++)
            StoreGuest32(block + i * 4, 0);

        lucent::debug("gpu", "VdSwap: swap packet at {:#x}, front buffer {:#x}", block,
                      frontBuffer);
    }

    ctx.r3.u64 = 0;
}

void __imp__VdQueryVideoFlags(PPCContext &__restrict ctx, uint8_t *)
{
    // Widescreen | HD, matching the 1280x720 mode XGetVideoMode reports.
    ctx.r3.u64 = 0x00000006;
}

void __imp__VdGetCurrentDisplayGamma(PPCContext &__restrict ctx, uint8_t *)
{
    StoreGuest32(ctx.r3.u32, 2);
    StoreGuest32(ctx.r4.u32, 0x40000000); // 2.0f
    ctx.r3.u64 = 0;
}

void __imp__VdGetCurrentDisplayInformation(PPCContext &__restrict ctx, uint8_t *)
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

void __imp__VdSetDisplayMode(PPCContext &__restrict ctx, uint8_t *)
{
    lucent::debug("gpu", "VdSetDisplayMode({:#x})", ctx.r3.u32);
    ctx.r3.u64 = 0;
}

void __imp__VdIsHSIOTrainingSucceeded(PPCContext &__restrict ctx, uint8_t *)
{
    // The high-speed IO link between CPU and GPU. There is no link to train.
    ctx.r3.u64 = 1;
}

void __imp__VdPersistDisplay(PPCContext &__restrict ctx, uint8_t *)
{
    StoreGuest32(ctx.r4.u32, 0);
    ctx.r3.u64 = 1;
}

void __imp__VdRetrainEDRAM(PPCContext &__restrict ctx, uint8_t *)
{
    // EDRAM is physical memory on the console's GPU daughter die; there is no
    // equivalent here and nothing to retrain.
    ctx.r3.u64 = 0;
}

void __imp__VdRetrainEDRAMWorker(PPCContext &__restrict ctx, uint8_t *)
{
    ctx.r3.u64 = 0;
}

void __imp__VdEnableDisableClockGating(PPCContext &__restrict ctx, uint8_t *)
{
    ctx.r3.u64 = 0;
}

void __imp__VdCallGraphicsNotificationRoutines(PPCContext &__restrict ctx, uint8_t *)
{
    ctx.r3.u64 = 0;
}

void __imp__VdQueryVideoMode(PPCContext &__restrict ctx, uint8_t *base)
{
    // Same mode XGetVideoMode reports; the two must not disagree.
    __imp__XGetVideoMode(ctx, base);
    ctx.r3.u64 = 0;
}

// The system command buffer is a small ring the driver writes into directly.
// It is handed out as real, committed memory; the packets the title writes
// into it (scratch write-back setup among them) are executed when the stream
// points an indirect buffer at it.
void __imp__VdGetSystemCommandBuffer(PPCContext &__restrict ctx, uint8_t *)
{
    static uint32_t s_commandBuffer = 0;
    if (s_commandBuffer == 0)
    {
        uint32_t size = 0x10000;
        s_commandBuffer =
            gears::PhysicalHeap().Allocate(0, size, gears::kMemCommit | gears::kMemLargePages);
        lucent::info("gpu", "system command buffer at {:#x}", s_commandBuffer);
    }

    StoreGuest32(ctx.r3.u32, s_commandBuffer);
    StoreGuest32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

void __imp__VdInitializeScalerCommandBuffer(PPCContext &__restrict ctx, uint8_t *)
{
    // Returns the number of command words written. Writing none is truthful.
    ctx.r3.u64 = 0;
}
