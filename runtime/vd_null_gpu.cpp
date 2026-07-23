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
#include "gpu_present.h"
#include "hle_d3d.h"
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
    uint32_t lastInnerCallback = 0xFFFFFFFF;

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
        // The source==1 arm of the title's ISR calls *(*(ctx+0x2A14)+0x10)
        // with *(*(ctx+0x2A14)+0x14); that inner callback is what signals the
        // D3D worker. A null slot means the ISR silently skips the signal, so
        // report the slot once per distinct value seen.
        if (source == 1)
        {
            const uint32_t inner =
                ReadGuest32(ReadGuest32(g_graphicsInterruptContext + 0x2A14) + 0x10);
            if (inner != lastInnerCallback)
            {
                lastInnerCallback = inner;
                // Pool base is *(*(0x82000868)); the per-CPU interrupt event
                // array the signalling callback (0x8223B8A0) sets lives at
                // pool+0x2BDC with a 0x38 stride.
                const uint32_t pool = ReadGuest32(ReadGuest32(0x82000868));
                lucent::debug("gpu",
                    "graphics ISR inner callback -> {:#x} (pool {:#x}, cpu{} event {:#x})",
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

// RB_SAMPLE_COUNT_ADDR: where EVENT_WRITE_ZPD writes its sample-count record.
constexpr uint32_t kRegSampleCountAddr = 0x2325;

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
    uint32_t type = 0;      // xenos::ShaderType: 0 vertex, 1 pixel
    uint32_t dwords = 0;
    uint32_t address = 0;   // physical address (IM_LOAD) or 0 (immediate)
    uint64_t loads = 0;
    bool immediate = false;
};

struct ShaderCaptureState
{
    bool enabled = false;
    bool ready = false;
    std::string dir = "scratch/shaders/bound";
    std::map<uint64_t, BoundShader> shaders; // ucode hash -> record
    uint64_t imLoads = 0;
    uint64_t imLoadsImmediate = 0;
    uint64_t truncated = 0;   // packet claimed more ucode than the buffer held
    uint64_t rejected = 0;    // implausible size
    uint64_t activeVertexHash = 0; // last vertex ucode bound (for the const dump)
    uint64_t activePixelHash = 0;  // last pixel ucode bound
} g_shaderCapture;

uint64_t Fnv1a64(const uint8_t* p, size_t n)
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
    const std::vector<uint8_t>& ucode)
{
    auto& cap = g_shaderCapture;
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
    cap.shaders.emplace(hash, s);

    const std::string path = std::format("{}/{}_{:016x}.ucode",
        cap.dir, type == 0 ? "vs" : "ps", hash);
    if (FILE* f = std::fopen(path.c_str(), "wb"))
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
    auto& cap = g_shaderCapture;
    const std::string path = cap.dir + "/manifest.csv";
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return;
    std::fprintf(f, "file,type,ucode_dwords,address,immediate,loads\n");
    for (const auto& [hash, s] : cap.shaders)
        std::fprintf(f, "%s_%016llx.ucode,%s,%u,0x%08X,%d,%llu\n",
            s.type == 0 ? "vs" : "ps", (unsigned long long)hash,
            s.type == 0 ? "vs" : "ps", s.dwords, s.address, s.immediate ? 1 : 0,
            (unsigned long long)s.loads);
    std::fclose(f);
}

void ShaderCaptureInit()
{
    auto& cap = g_shaderCapture;
    if (cap.ready)
        return;
    cap.ready = true;
    cap.enabled = lucent::config::flag("SHADER_CAPTURE");
    if (!cap.enabled)
        return;
    const std::string& dir = lucent::config::text("SHADER_CAPTURE_DIR");
    if (!dir.empty())
        cap.dir = dir;
    if (std::system(("mkdir -p '" + cap.dir + "'").c_str()) != 0)
        lucent::warn("gpu", "shader capture: cannot create {}", cap.dir);
    lucent::info("gpu", "shader capture armed (PM4 IM_LOAD), writing microcode to {}", cap.dir);
}

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
    // SCRATCH_ADDR/SCRATCH_UMSK retarget every subsequent write-back, so a
    // stray write to either silently redirects the ISR's callback slot and the
    // stream's completion flags. Report every change to them.
    if ((reg == kRegScratchAddr || reg == kRegScratchUmsk) && g_gpuRegisters[reg] != value)
        lucent::debug("gpu", "SCRATCH_{} {:#x} -> {:#x}",
            reg == kRegScratchAddr ? "ADDR" : "UMSK", g_gpuRegisters[reg], value);
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

// Names only for the opcodes this investigation reasons about; anything else
// prints as its number, which is enough to look up in Xenia's xenos.h.
std::string OpcodeName(uint32_t op)
{
    switch (op)
    {
    case 0x10: return "NOP";
    case 0x22: return "DRAW_INDX";
    case 0x23: return "VIZ_QUERY";
    case 0x25: return "SET_STATE";
    case 0x26: return "WAIT_FOR_IDLE";
    case 0x2D: return "SET_CONSTANT";
    case 0x2F: return "LOAD_ALU_CONSTANT";
    case 0x36: return "DRAW_INDX_2";
    case 0x37: return "IB_PFD";
    case 0x3B: return "INVALIDATE_STATE";
    case 0x3C: return "WAIT_REG_MEM";
    case 0x3D: return "MEM_WRITE";
    case 0x3F: return "IB";
    case 0x44: return "COND_EXEC";
    case 0x45: return "COND_WRITE";
    case 0x46: return "EVENT_WRITE";
    case 0x4B: return "SET_BIN_BASE_OFFSET";
    case 0x50: return "SET_BIN_MASK";
    case 0x51: return "SET_BIN_SELECT";
    case 0x54: return "INTERRUPT";
    case 0x55: return "SET_CONSTANT2";
    case 0x56: return "SET_SHADER_CONSTANTS";
    case 0x58: return "EVENT_WRITE_SHD";
    case 0x5A: return "EVENT_WRITE_EXT";
    case 0x5B: return "EVENT_WRITE_ZPD";
    case 0x60: return "SET_BIN_MASK_LO";
    case 0x61: return "SET_BIN_MASK_HI";
    case 0x62: return "SET_BIN_SELECT_LO";
    case 0x63: return "SET_BIN_SELECT_HI";
    case kOpRuntimeSwap: return "SWAP";
    default: return std::format("op{:#x}", op);
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
    uint64_t setConstantByType[8]{};   // SET_CONSTANT (0x2D) by type field
    uint64_t loadAluConstantByType[8]{}; // LOAD_ALU_CONSTANT (0x2F) by type field
    uint64_t setConstant2Packets = 0;
    uint64_t setShaderConstantsPackets = 0;
    uint64_t type0AluWrites = 0;        // TYPE0 dwords into 0x4000..0x47FF
    uint64_t type0FetchWrites = 0;      // TYPE0 dwords into 0x4800..0x48FF
    bool constDumpDone = false;         // one-shot verification dump latch
    bool drawCaptureDone = false;       // one-shot hot-pair draw-param capture latch

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
    std::map<uint32_t, uint64_t> predicatedSeen;   // opcode -> predicated packets
    std::map<uint32_t, uint64_t> predicatedSkip;   // opcode -> would be skipped
    uint64_t predicateOffPackets = 0;              // any packet while select&mask==0

    // The whole of the CP thread's wall time between frames, split into the
    // three places it can go. Anything unaccounted for is the guest's own
    // execution time, which is the point of the split: it says whether a slow
    // frame is our command processor or the title.
    std::chrono::steady_clock::time_point frameStart = std::chrono::steady_clock::now();
    uint64_t idleUs = 0;      // ring empty: waiting for the title to submit
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
        for (const auto& [addr, stat] : waitStats)
        {
            if (stat.second > 1000)
                lucent::debug("gpu", "  waits on {:#x}: {} times, {} ms",
                    addr, stat.first, stat.second / 1000);
        }
        uint64_t ibTotal = 0;
        uint64_t ibMax = 0;
        for (const auto& [addr, n] : ibCounts)
        {
            ibTotal += n;
            ibMax = std::max(ibMax, n);
        }
        lucent::debug("gpu", "frame packets: {} IB submissions of {} distinct buffers"
            " (max {} each); ring dwords written {} consumed {}",
            ibTotal, ibCounts.size(), ibMax, frameWptrAdvance, frameRptrAdvance);
        frameWptrAdvance = frameRptrAdvance = 0;
        lucent::Line ring;
        ring.add("  ring TYPE3:");
        for (const auto& [op, n] : ringOpcodes)
            ring.add(" {}x{}", OpcodeName(op), n);
        ring.flush_debug("gpu");
        lucent::Line inner;
        inner.add("  IB TYPE3:");
        for (const auto& [op, n] : innerOpcodes)
            inner.add(" {}x{}", OpcodeName(op), n);
        inner.flush_debug("gpu");

        if (lucent::config::flag("CONST_DUMP"))
            lucent::debug("gpu", "  constant feed (cumulative): SET_CONSTANT"
                "[alu {} fetch {} bool {} loop {} reg {}] LOAD_ALU_CONSTANT[alu {} fetch {}]"
                " SET_CONSTANT2 {} SET_SHADER_CONSTANTS {} TYPE0[alu {} fetch {}]",
                setConstantByType[0], setConstantByType[1], setConstantByType[2],
                setConstantByType[3], setConstantByType[4], loadAluConstantByType[0],
                loadAluConstantByType[1], setConstant2Packets, setShaderConstantsPackets,
                type0AluWrites, type0FetchWrites);

        lucent::Line pred;
        pred.add("  predication: select {:#x} mask {:#x}; packets seen while OFF {};"
            " predicated packets", binSelect, binMask, predicateOffPackets);
        for (const auto& [op, n] : predicatedSeen)
            pred.add(" {}x{}(skip {})", OpcodeName(op), n,
                predicatedSkip.count(op) ? predicatedSkip[op] : 0);
        pred.flush_debug("gpu");
        predicatedSeen.clear();
        predicatedSkip.clear();
        predicateOffPackets = 0;

        if (g_shaderCapture.enabled)
        {
            const auto& cap = g_shaderCapture;
            size_t vs = 0, ps = 0;
            uint64_t maxLoads = 0;
            for (const auto& [hash, s] : cap.shaders)
            {
                (s.type == 0 ? vs : ps)++;
                maxLoads = std::max(maxLoads, s.loads);
            }
            lucent::debug("gpu", "shader capture: {} IM_LOAD + {} IM_LOAD_IMMEDIATE, "
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
    void CaptureShaderLoad(uint32_t opcode, Fetch&& fetch, uint32_t usable, uint32_t count)
    {
        auto& cap = g_shaderCapture;
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
        if (type > 1 || start != 0 || sizeDwords == 0 || sizeDwords % 3 != 0 ||
            sizeDwords > 0x4000)
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
    static bool ConstFileBase(uint32_t type, uint32_t& base)
    {
        switch (type)
        {
        case 0: base = kConstBaseAlu; return true;       // ALU float
        case 1: base = kConstBaseFetch; return true;     // vertex/texture fetch
        case 2: base = kConstBaseBool; return true;      // bool
        case 3: base = kConstBaseLoop; return true;      // loop
        case 4: base = kConstBaseRegisters; return true; // general registers
        default: return false;
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
    void TrackConstantLoad(uint32_t opcode, Fetch&& fetch, uint32_t usable, uint32_t count)
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
                lucent::warn("gpu", "SET_CONSTANT unknown type {} (offset_type {:#x})",
                    type, offsetType);
                return;
            }
            const uint32_t n = count - 1; // constant dwords after offset_type
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
                lucent::warn("gpu", "LOAD_ALU_CONSTANT unknown type {} (offset_type {:#x})",
                    type, offsetType);
                return;
            }
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
            (opcode == kOpSetConstant2 ? setConstant2Packets
                                       : setShaderConstantsPackets)++;
            const uint32_t n = count - 1;
            for (uint32_t i = 0; i < n && (1 + i) < usable; ++i)
                WriteGpuRegister(index + i, fetch(1 + i));
            break;
        }
        default:
            break;
        }
    }

    // One-shot verification dump of the constant files, so the captured bytes
    // can be checked against Xenia's expectations. Targets the hot pair's
    // vertex shader (vs_5363d074) by default, whose disassembly reads vertex
    // fetch constant vf0 (Stride=12) -- so vf0 at register 0x4800 is decoded as
    // xe_gpu_vertex_fetch_t and the texture slots as xe_gpu_texture_fetch_t.
    // Gated on GEARS_CONST_DUMP; GEARS_CONST_DUMP_ANY drops the hot-pair filter.
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
        lucent::info("gpu", "constant dump at {} -- feed census:"
            " SET_CONSTANT[alu {} fetch {} bool {} loop {} reg {}]"
            " LOAD_ALU_CONSTANT[alu {} fetch {}] SET_CONSTANT2 {} SET_SHADER_CONSTANTS {}"
            " TYPE0[alu {} fetch {}]",
            OpcodeName(drawOpcode), setConstantByType[0], setConstantByType[1],
            setConstantByType[2], setConstantByType[3], setConstantByType[4],
            loadAluConstantByType[0], loadAluConstantByType[1],
            setConstant2Packets, setShaderConstantsPackets,
            type0AluWrites, type0FetchWrites);

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
            const uint32_t* s = &g_gpuRegisters[kConstBaseFetch + slot * 6];
            const uint32_t slotType = s[0] & 3;
            if (slotType == 2) // xe_gpu_texture_fetch_t
            {
                ++textureSlots;
                const uint32_t base = (s[1] >> 12) << 12;
                const uint32_t pitch = ((s[0] >> 22) & 0x1FF) << 5;
                const uint32_t width = (s[2] & 0x1FFF) + 1;   // size_2d.width
                const uint32_t height = ((s[2] >> 13) & 0x1FFF) + 1; // size_2d.height
                const uint32_t format = s[1] & 0x3F;
                const uint32_t tiled = s[0] >> 31;
                const uint32_t endian = (s[1] >> 6) & 3;
                if (textureSlots <= 12)
                    lucent::info("gpu", "  texfetch[slot {}] (reg {:#x}): base {:#x} {}x{}"
                        " pitch {} px format {:#x} tiled {} endian {}", slot,
                        kConstBaseFetch + slot * 6, base, width, height, pitch, format,
                        tiled, endian);
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
                    lucent::info("gpu", "  vfetch const #{} (reg {:#x}): {:#010x} {:#010x} ->"
                        " base {:#x} size {} words ({} bytes) endian {}",
                        slot * 3 + j, kConstBaseFetch + slot * 6 + j * 2, d0, d1,
                        byteAddr, sizeWords, sizeWords * 4, endian);
                }
            }
        }
        lucent::info("gpu", "  fetch file: {} texture slots, {} vertex-fetch constants"
            " (type 3)", textureSlots, vertexConsts);

        // ALU float constants: 256 vec4 at 0x4000. Count non-zero dwords and show
        // the first few vec4 so the transform matrices are visibly present.
        uint32_t nonZeroAlu = 0;
        for (uint32_t i = 0; i < 1024; ++i)
            if (g_gpuRegisters[kConstBaseAlu + i] != 0)
                ++nonZeroAlu;
        lucent::info("gpu", "  ALU float constants non-zero: {} of 1024 dwords", nonZeroAlu);
        for (uint32_t v = 0; v < 6; ++v)
        {
            const uint32_t* p = &g_gpuRegisters[kConstBaseAlu + v * 4];
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
            const char* dir = std::getenv("GEARS_CONST_DUMP_DIR");
            fs::path outdir = dir ? fs::path(dir) : fs::path("scratch/bin");
            std::error_code ec;
            fs::create_directories(outdir, ec);
            const fs::path out = outdir / "regfile_hotpair.bin";
            std::ofstream f(out, std::ios::binary);
            if (f)
            {
                f.write(reinterpret_cast<const char*>(g_gpuRegisters.data()),
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
    // The vertex geometry source is NOT in the draw packet: the hot VS fetches it
    // from the shared-memory SSBO using vertex fetch constant #95 (Xenia disasm
    // "vf0" == 95 - storage_index; verified via spirv-dis: the shader reads
    // fetch_constants[0][47][2..3] == fetch-file dwords 190,191 == register
    // 0x48BE/0x48BF) with a baked Stride=12 dwords (48 bytes). Base address comes
    // from that fetch constant's dword_0 (address = dword_0 >> 2, in dwords).
    static constexpr uint32_t kHotVertexFetchIndex = 95;   // from vf0 disasm
    static constexpr uint32_t kHotVertexStrideDwords = 12; // baked Stride=12

    static uint16_t SwapIndex16(uint16_t v, uint32_t endian)
    {
        // xenos::Endian: k8in16 (1) swaps the two bytes of each 16-bit index.
        return endian == 1 ? uint16_t((v >> 8) | (v << 8)) : v;
    }

    void CaptureHotDraw(uint32_t opcode, const uint32_t* raw, uint32_t usable)
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
        const char* dir = std::getenv("GEARS_DRAW_CAPTURE_DIR");
        fs::path outdir = dir ? fs::path(dir) : fs::path("scratch/draw-params");
        std::error_code ec;
        fs::create_directories(outdir, ec);
        std::ofstream rep(outdir / "hot_draw.txt");

        auto emit = [&](const std::string& s) {
            lucent::info("gpu", "{}", s);
            if (rep) rep << s << '\n';
        };

        static const char* kPrim[16] = {
            "none", "point_list", "line_list", "line_strip", "triangle_list",
            "triangle_fan", "triangle_strip", "triangle_w_wflags",
            "rectangle_list", "unused1", "unused2", "unused3", "line_loop",
            "quad_list", "quad_strip", "polygon"};
        static const char* kSrc[4] = {"kDMA(indexed)", "kImmediate(inline)",
                                      "kAutoIndex", "invalid"};

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
                " (num_words {}, swap_mode {})", dmaBase, dmaSize, dmaNumWords, dmaSwap));
            emit(std::format("  index buffer: guest_base {:#x}, {} indices,"
                " {}-bit, {} bytes, endian {}", indexGuestBase, numIndices,
                indexSizeBytes * 8, indexLenBytes, dmaSwap));
        }
        else if (sourceSelect == 2)
        {
            emit("index source: kAutoIndex (no index buffer; indices 0..num_indices-1)");
        }

        // Geometry source: vertex fetch constant #95 (the hot VS's vf0).
        const uint32_t vfType = vf0 & 0x3;
        const uint32_t vertexBaseBytes = (vf0 >> 2) << 2; // address<<2, byte addr
        const uint32_t vfEndian = vf1 & 0x3;
        const uint32_t vfSizeWords = (vf1 >> 2) & 0xFFFFFF;
        const uint32_t strideBytes = kHotVertexStrideDwords * 4;
        emit(std::format("vertex fetch constant #95 (reg {:#x}): {:#010x} {:#010x}",
            fetchBase, vf0, vf1));
        emit(std::format("  type {} vertex_base {:#x} (dword addr {:#x}) stride {} dwords"
            " ({} bytes) endian {} size {} words ({} bytes)", vfType, vertexBaseBytes,
            vf0 >> 2, kHotVertexStrideDwords, strideBytes, vfEndian, vfSizeWords,
            vfSizeWords * 4));

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
                    const uint16_t* p = gears::Memory().Translate<uint16_t>(
                        indexGuestBase + i * 2);
                    idx = SwapIndex16(*p, dmaSwap);
                }
                else
                    idx = ReadGuest32(indexGuestBase + i * 4);
                if (i < nIdx) { indices.push_back(idx); line.add(" {}", idx); }
                minI = std::min(minI, idx);
                maxI = std::max(maxI, idx);
            }
            line.flush(lucent::Level::Info, "gpu");
            if (rep)
            {
                rep << "first " << nIdx << " indices:";
                for (uint32_t v : indices) rep << ' ' << v;
                rep << "\n";
            }
            emit(std::format("  index range: min {} max {} (vertex-buffer size {} bytes"
                " admits index < {})", minI, maxI, vfSizeWords * 4,
                strideBytes ? vfSizeWords * 4 / strideBytes : 0));
        }

        // First few vertices from the shared-memory SSBO source. The hot VS's
        // first vfetch_full is FMT_32_32_32_32_FLOAT at dword offset 0 of the
        // vertex, so dwords 0..3 are the position attribute (x,y,z,w) in floats.
        const uint32_t nVtx = 6;
        emit(std::format("first {} vertices (dwords 0..3 = FMT_32_32_32_32_FLOAT"
            " position attribute):", nVtx));
        // Which vertices to sample: the first few referenced indices, or 0..n.
        std::vector<uint32_t> sampleVerts;
        if (!indices.empty())
            for (uint32_t i = 0; i < nVtx && i < indices.size(); ++i)
                sampleVerts.push_back(indices[i]);
        else
            for (uint32_t i = 0; i < nVtx; ++i) sampleVerts.push_back(i);

        for (uint32_t vi : sampleVerts)
        {
            const uint32_t vaddr = vertexBaseBytes + vi * strideBytes;
            uint32_t d[12];
            for (uint32_t k = 0; k < 12; ++k)
            {
                const uint32_t w = ReadGuest32(vaddr + k * 4); // k8in32 == full swap
                // vf endian: 2 (k8in32) matches ReadGuest32's full byteswap.
                d[k] = (vfEndian == 2) ? w : ReadGuest32Raw(vaddr + k * 4, vfEndian);
            }
            float pos[4];
            for (int k = 0; k < 4; ++k) std::memcpy(&pos[k], &d[k], 4);
            emit(std::format("  v[{}] @ {:#x}: pos ({}, {}, {}, {})  raw"
                " {:#010x} {:#010x} {:#010x} {:#010x}", vi, vaddr,
                pos[0], pos[1], pos[2], pos[3], d[0], d[1], d[2], d[3]));
        }
        emit(std::format("(wrote to {})", (outdir / "hot_draw.txt").string()));
    }

    // Read a guest dword applying an explicit xenos::Endian swap, for vertex
    // data whose fetch-constant endian differs from the ring's default.
    static uint32_t ReadGuest32Raw(uint32_t addr, uint32_t endian)
    {
        const uint8_t* p = gears::Memory().Translate<uint8_t>(addr);
        const uint32_t b = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        switch (endian)
        {
        case 1: // k8in16
            return ((b & 0x00FF00FF) << 8) | ((b & 0xFF00FF00) >> 8);
        case 2: // k8in32
            return __builtin_bswap32(b);
        case 3: // k16in32
            return ((b & 0x0000FFFF) << 16) | ((b & 0xFFFF0000) >> 16);
        default:
            return b;
        }
    }

    void HandleType3(uint32_t opcode, const uint32_t* data, uint32_t count, int depth)
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
            if (count >= 1) binMask = (binMask & 0xFFFFFFFF00000000ull) | data[0];
            break;
        case 0x61:
            if (count >= 1) binMask = (binMask & 0xFFFFFFFFull) | (uint64_t(data[0]) << 32);
            break;
        case 0x62:
            if (count >= 1) binSelect = (binSelect & 0xFFFFFFFF00000000ull) | data[0];
            break;
        case 0x63:
            if (count >= 1) binSelect = (binSelect & 0xFFFFFFFFull) | (uint64_t(data[0]) << 32);
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
                        lucent::debug("gpu", "IB {:#x} ({} words) from ring dword {:#x}",
                            address, words, sourceIndex);
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
                    lucent::debug("gpu", "swap packet: front buffer {:#x} (seq {})",
                        data[0], data[1]);
                    ReportWaitStats();
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
                    lucent::debug("gpu", "stale swap packet ignored (seq {} <= {})",
                        data[1], lastSwapSequence);
                }
            }
            break;

        case kOpEventWriteZpd:
            // Z-pass-done event: the GPU writes an xe_gpu_depth_sample_counts
            // record (0x20 bytes: Total/ZFail/ZPass/StencilFail A+B pairs,
            // little-endian) at the record the address in RB_SAMPLE_COUNT_ADDR
            // selects. D3D pre-fills records with the 0xFFFFFEED sentinel and
            // its occlusion-query GetData (0x822306A0) polls until the event
            // overwrites it -- the post-load no-present spin was exactly this
            // poll against a record no one wrote. A GPU that rasterises
            // nothing has zero samples in every counter, so the record is
            // zero-filled, which also clears the sentinel. Layout and
            // addressing per Xenia's xenos_zpd_report.h (record = addr &
            // ~0x1F; END at slot+0, BEGIN at slot+0x20).
            {
                const uint32_t reportAddress = g_gpuRegisters[kRegSampleCountAddr];
                const uint32_t recordBase = reportAddress & ~0x1Fu;
                if (recordBase != 0)
                {
                    uint8_t* record = gears::Memory().Translate<uint8_t>(recordBase);
                    memset(record, 0, 0x20);
                    lucent::debug("gpu", "EVENT_WRITE_ZPD: zero samples -> {:#x}"
                        " (initiator {:#x})", recordBase, count >= 1 ? data[0] : 0u);
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
                    0,            // min x (in 8px blocks)
                    8192 >> 3,    // max x
                    0,            // min y
                    8192 >> 3,    // max y
                    0,            // min z
                    1,            // max z
                };
                auto* out = gears::Memory().Translate<uint16_t>(address);
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
                        interruptUs += uint64_t(
                            std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - isrStart).count());
                        ++interrupts;
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
        auto& stat = waitStats[(waitInfo & 0x10) ? (poll & ~3u) : (poll & 0x7FFF)];
        ++stat.first;
        const auto accumulate = [&] {
            const uint64_t us = uint64_t(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count());
            stat.second += us;
            regWaitUs += us;
        };
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
            {
                accumulate();
                return;
            }

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
        if (opcode == kOpSetConstant || opcode == kOpLoadAluConstant ||
            opcode == kOpSetConstant2 || opcode == kOpSetShaderConstants)
            TrackConstantLoad(opcode, fetch, usable, count);

        // Verification hook: dump the constant files at the first real draw.
        if (opcode == 0x22 || opcode == 0x36) // DRAW_INDX / DRAW_INDX_2
        {
            DumpConstantFiles(opcode);
            CaptureHotDraw(opcode, data, copy);
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

        lucent::info("gpu", "command processor consuming ring {:#x} ({} bytes)",
            g_ringBuffer.base, g_ringBuffer.Bytes());

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
                    std::chrono::steady_clock::now() - idleStart).count());
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
            const uint32_t consumed = ExecutePacket(header, [&](uint32_t w) {
                return ReadGuest32(g_ringBuffer.base + ((rptr + w) & (dwords - 1)) * 4);
            }, dwords, 0);

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
                lucent::error("gpu", "packet at ring dword {:#x} claims {} dwords with only"
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
            lucent::debug("ring", "{:#06x} {:#010x} +{} wptr {:#06x}->{:#06x}",
                ringIndex, header, consumed + 1, wptr,
                ReadGuest32(kCpRbWptr) & (dwords - 1));

            rptrTotal += consumed + 1;
            frameRptrAdvance += consumed + 1;
            if (!overshootReported && rptrTotal > wptrTotal)
            {
                overshootReported = true;
                lucent::error("gpu", "ring overshoot: consumed {} dwords but only {} written;"
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
    gears::PresenterStart();

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
    gears::HleDumpCensus("swap");
    gears::HleWorkerCensus();
    gears::HleShaderCaptureFrame(frame);
    if (frame == 1 || frame % 60 == 0)
    {
        // Frame rate is the metric the presentation work is judged on, so
        // report it measured rather than leaving it to be inferred from log
        // line counts (the log has no timestamps).
        static std::chrono::steady_clock::time_point last;
        const auto now = std::chrono::steady_clock::now();
        const double seconds =
            frame == 1 ? 0.0 : std::chrono::duration<double>(now - last).count();
        last = now;
        lucent::info("gpu", "VdSwap: {} frames submitted, last 60 in {:.2f}s ({:.2f} fps)",
            frame, seconds, seconds > 0 ? 60.0 / seconds : 0.0);
    }

    const uint32_t block = ctx.r3.u32;
    if (block != 0)
    {
        const uint32_t frontBuffer =
            ctx.r8.u32 != 0 ? ReadGuest32(ctx.r8.u32) : 0;

        StoreGuest32(block, (3u << 30) | ((kSwapReservationDwords - 2) << 16)
            | (kOpRuntimeSwap << 8));
        StoreGuest32(block + 4, frontBuffer);
        StoreGuest32(block + 8, uint32_t(frame)); // sequence, see kOpRuntimeSwap
        for (uint32_t i = 3; i < kSwapReservationDwords; i++)
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
