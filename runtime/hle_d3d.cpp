// Native overrides of guest D3D functions, and the instrumentation used to
// derive their contract.
//
// Mechanism: XenonRecomp emits every translated function as
//     __attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X);
//     PPC_FUNC_IMPL(__imp__sub_X) { ... }
// so a STRONG definition of `sub_X` in this translation unit replaces the
// guest body at every direct call site, while `__imp__sub_X` stays reachable
// as a super-call. Nothing here goes through PPCFuncMappings: that table only
// serves indirect calls, and the D3D layer is called with plain `bl`.
#include "import_stub.h"

#include <atomic>
#include <cstdint>
#include <lucent/config.h>
#include <lucent/log.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "guest_memory.h"
#include "hle_d3d.h"

// ---------------------------------------------------------------------------
// Registration helper. GEARS_HLE_TRACE(addr) defines a strong sub_<addr> that
// counts the call, records the return address (= the call site, which is the
// only caller provenance the translated code carries), and chains to the guest
// body.
// ---------------------------------------------------------------------------

namespace
{

uint32_t ReadGuestBE32(uint32_t address)
{
    return __builtin_bswap32(*gears::Memory().Translate<uint32_t>(address));
}

struct CallSite
{
    uint32_t lr = 0;
    uint64_t count = 0;
};

struct Probe
{
    const char* name;
    uint32_t address;
    std::atomic<uint64_t> calls{0};
    CallSite sites[8]{};
};

constexpr size_t kMaxProbes = 32;
Probe* g_probes[kMaxProbes]{};
std::atomic<size_t> g_probeCount{0};

void Register(Probe* p)
{
    size_t i = g_probeCount.fetch_add(1);
    if (i < kMaxProbes)
        g_probes[i] = p;
}

void Note(Probe& p, uint32_t lr)
{
    p.calls.fetch_add(1, std::memory_order_relaxed);
    for (auto& s : p.sites)
    {
        if (s.lr == lr) { ++s.count; return; }
        if (s.lr == 0) { s.lr = lr; s.count = 1; return; }
    }
}

} // namespace

namespace gears
{

void HleDumpCensus(const char* why)
{
    size_t n = g_probeCount.load();
    static bool announced = false;
    if (!announced)
    {
        announced = true;
        lucent::info("hle", "{} probes registered", n);
    }
    for (size_t i = 0; i < n && i < kMaxProbes; ++i)
    {
        Probe* p = g_probes[i];
        if (!p || p->calls.load() == 0)
            continue;
        lucent::Line line;
        line.add("{} sub_{} calls={}", why, p->name, p->calls.load());
        for (const auto& s : p->sites)
        {
            if (s.lr == 0) break;
            line.add("  from {:#x} x{}", s.lr, s.count);
        }
        line.flush_debug("hle");
    }
}

} // namespace gears

#define GEARS_HLE_TRACE(addr)                                                  \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                    \
    namespace {                                                                \
    Probe g_probe_##addr{#addr, 0x##addr};                                     \
    struct Reg_##addr { Reg_##addr() { Register(&g_probe_##addr); } }          \
        g_reg_##addr;                                                          \
    }                                                                          \
    PPC_FUNC(sub_##addr)                                                       \
    {                                                                          \
        Note(g_probe_##addr, uint32_t(ctx.lr));                                \
        __imp__sub_##addr(ctx, base);                                          \
    }

// The submission chain, bottom to top.
GEARS_HLE_TRACE(822218C0) // submit: direct kick, or record into the CPU list
GEARS_HLE_TRACE(82221980) // flush / end-of-segment
GEARS_HLE_TRACE(8223BA18) // frame-boundary block seen bracketing submissions
GEARS_HLE_TRACE(82221A68) // ticket fence wait
GEARS_HLE_TRACE(8223B200) // CPU command-list interpreter
GEARS_HLE_TRACE(8223E3E0) // Present
GEARS_HLE_TRACE(8223E860) // present / retire pump

// ---------------------------------------------------------------------------
// The worker's replay queue.
//
// sub_8223B5E0 takes the queued CPU command list from context+0x58 (clearing
// it) and interprets it. In the scene phase it runs 392 times per presented
// frame while the D3D API submits only 8 times, so either the same list is
// re-queued or the take does not stick. This records the list head each run.
// ---------------------------------------------------------------------------

namespace
{

uint32_t g_lastQueueHead = 0;
uint64_t g_repeatRuns = 0;
uint64_t g_distinctRuns = 0;
uint64_t g_emptyRuns = 0;

// The enqueue side. The watchpoint identified sub_8223B8A0 -- the GPU interrupt
// callback -- as the only writer of the queue besides the interpreter's own
// clear. This censuses the list pointers it enqueues, and whether an enqueue
// overwrites a list the worker has not consumed yet.
struct ValueSlot
{
    uint32_t value = 0;
    uint64_t count = 0;
};
constexpr size_t kValueSlots = 64;
ValueSlot g_enqueued[kValueSlots]{};
// The ring-kick side: word 0 of each 2-word submission descriptor is the
// indirect buffer's size in dwords, which identifies the buffer.
ValueSlot g_headers[kValueSlots]{};
uint64_t g_kicks = 0;
uint64_t g_enqueues = 0;
uint64_t g_enqueueOverwrites = 0;
uint64_t g_enqueueSelf = 0;
uint64_t g_enqueueDistinct = 0;
uint32_t g_currentList = 0;
uint32_t g_queueContext = 0;

// Replay progress. The interpreter suspends on a GPU-sync token, leaving a
// resume pointer at ctx+0x50, and is resumed when the ISR re-enqueues the list.
// A replay that keeps starting from the SAME (list, resume) point is making no
// progress; one that advances is a legitimate coroutine.
struct ReplaySlot
{
    uint32_t list = 0;
    uint32_t resume = 0;
    uint64_t count = 0;
};
constexpr size_t kReplaySlots = 64;
ReplaySlot g_replays[kReplaySlots]{};
uint64_t g_replayTotal = 0;
uint64_t g_replayOverflow = 0;
uint64_t g_replayCompleted = 0;
uint64_t g_replaySuspended = 0;

void CountReplay(uint32_t list, uint32_t resume)
{
    ++g_replayTotal;
    for (auto& s : g_replays)
    {
        if (s.count && s.list == list && s.resume == resume) { ++s.count; return; }
        if (s.count == 0) { s.list = list; s.resume = resume; s.count = 1; return; }
    }
    ++g_replayOverflow;
}

void CountValue(ValueSlot* slots, size_t n, uint32_t value)
{
    for (size_t i = 0; i < n; ++i)
    {
        if (slots[i].value == value) { ++slots[i].count; return; }
        if (slots[i].count == 0) { slots[i].value = value; slots[i].count = 1; return; }
    }
}

void WatchReport();

} // namespace

namespace gears
{

void HleWorkerCensus()
{
    lucent::debug("hle", "worker replays: {} with a new list, {} repeating the "
                         "previous list, {} with an empty queue",
        g_distinctRuns, g_repeatRuns, g_emptyRuns);
    {
        lucent::Line line;
        for (const auto& s : g_enqueued)
        {
            if (s.count == 0) break;
            ++g_enqueueDistinct;
        }
        line.add("queue enqueues by sub_8223B8A0 this frame: {} total, {} distinct lists, {} "
                 "overwrote an unconsumed list, {} re-enqueued the list being replayed",
            g_enqueues, g_enqueueDistinct, g_enqueueOverwrites, g_enqueueSelf);
        size_t shown = 0;
        for (const auto& s : g_enqueued)
        {
            if (s.count == 0) break;
            if (shown++ < 16)
                line.add("  list {:#x} x{}", s.value, s.count);
        }
        line.flush_debug("hle");
        // Per-frame deltas: clear so the next census describes one frame.
        for (auto& s : g_enqueued)
            s = ValueSlot{};
        g_enqueues = g_enqueueOverwrites = g_enqueueSelf = g_enqueueDistinct = 0;
    }
    {
        lucent::Line line;
        line.add("ring kicks this frame: {}", g_kicks);
        size_t shown = 0;
        for (const auto& s : g_headers)
        {
            if (s.count == 0) break;
            if (shown++ < 16)
                line.add("  IB size {} x{}", s.value, s.count);
        }
        line.flush_debug("hle");
        for (auto& s : g_headers)
            s = ValueSlot{};
        g_kicks = 0;
    }
    {
        lucent::Line line;
        size_t distinct = 0, maxCount = 0;
        for (const auto& s : g_replays)
        {
            if (s.count == 0) break;
            ++distinct;
            if (s.count > maxCount) maxCount = s.count;
        }
        line.add("replay progress this frame: {} replays over {} distinct (list,resume) "
                 "points, worst point repeated {}x, {} overflowed the table; {} suspended, "
                 "{} ran to completion",
            g_replayTotal, distinct, maxCount, g_replayOverflow, g_replaySuspended,
            g_replayCompleted);
        size_t shown = 0;
        for (const auto& s : g_replays)
        {
            if (s.count == 0) break;
            if (shown++ < 12)
                line.add("  list {:#x} resume {:#x} x{}", s.list, s.resume, s.count);
        }
        line.flush_debug("hle");
        for (auto& s : g_replays)
            s = ReplaySlot{};
        g_replayTotal = g_replayOverflow = g_replaySuspended = g_replayCompleted = 0;
    }
    WatchReport();
}

} // namespace gears

// ---------------------------------------------------------------------------
// A guest-memory write watchpoint.
//
// Nothing static locates the producer of the worker's queue: the only store to
// <ctx>+0x58 in the image is the interpreter's own clear, so the enqueue must
// reach the field through a different base pointer (a different structure
// offset). The only way to name the writer is to catch the write.
//
// Mechanism: mprotect the page holding the field read-only and take the write
// fault. The faulting host RIP is inside the recompiled function that performed
// the store, so `sub_XXXXXXXX` falls out of the host symbol. After recording,
// the page is reopened and the faulting instruction retried under the x86 trap
// flag, so the page can be closed again on the resulting SIGTRAP. Handlers do
// no allocation and no logging; samples are printed from the census.
//
// Enabled with GEARS_WATCH_QUEUE=1. Disarms itself after kWatchSamples hits.
// ---------------------------------------------------------------------------

namespace
{

constexpr size_t kWatchSamples = 64;
constexpr size_t kWatchSlots = 16;

struct WatchSlot
{
    uintptr_t rip = 0;
    uint64_t count = 0;
};

struct Watch
{
    std::atomic<bool> armed{false};
    uint8_t* page = nullptr;
    size_t pageSize = 0;
    uint8_t* target = nullptr;
    uint32_t guestTarget = 0;
    std::atomic<uint64_t> hits{0};
    std::atomic<uint64_t> otherFaults{0};
    WatchSlot slots[kWatchSlots]{};
    uintptr_t moduleBase = 0;
    struct sigaction oldSegv{};
    struct sigaction oldTrap{};
} g_watch;

void WatchRecord(uintptr_t rip)
{
    for (auto& s : g_watch.slots)
    {
        if (s.rip == rip) { ++s.count; return; }
        if (s.rip == 0) { s.rip = rip; s.count = 1; return; }
    }
}

void WatchProtect(int prot)
{
    mprotect(g_watch.page, g_watch.pageSize, prot);
}

void OnSegv(int sig, siginfo_t* info, void* uc)
{
    auto* addr = static_cast<uint8_t*>(info->si_addr);
    if (!g_watch.armed.load(std::memory_order_relaxed) || addr < g_watch.page ||
        addr >= g_watch.page + g_watch.pageSize)
    {
        // Not ours: restore the previous disposition and let it fault again.
        sigaction(SIGSEGV, &g_watch.oldSegv, nullptr);
        return;
    }

    auto* ctx = static_cast<ucontext_t*>(uc);
    if (addr >= g_watch.target && addr < g_watch.target + 4)
    {
        WatchRecord(uintptr_t(ctx->uc_mcontext.gregs[REG_RIP]));
        g_watch.hits.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        g_watch.otherFaults.fetch_add(1, std::memory_order_relaxed);
    }

    WatchProtect(PROT_READ | PROT_WRITE);
    if (g_watch.hits.load(std::memory_order_relaxed) >= kWatchSamples)
    {
        g_watch.armed.store(false, std::memory_order_relaxed);
        return; // leave the page open; the census reports what was caught
    }
    ctx->uc_mcontext.gregs[REG_EFL] |= 0x100; // single-step the retried store
}

void OnTrap(int sig, siginfo_t* info, void* uc)
{
    (void)sig;
    (void)info;
    auto* ctx = static_cast<ucontext_t*>(uc);
    ctx->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    if (g_watch.armed.load(std::memory_order_relaxed))
        WatchProtect(PROT_READ);
}

void WatchArm(uint32_t guestAddress)
{
    static bool tried = false;
    if (tried)
        return;
    tried = true;
    if (!lucent::config::flag("WATCH_QUEUE"))
        return;

    uint8_t* host = gears::Memory().Translate<uint8_t>(guestAddress);
    const size_t pageSize = size_t(sysconf(_SC_PAGESIZE));
    g_watch.pageSize = pageSize;
    g_watch.page = reinterpret_cast<uint8_t*>(uintptr_t(host) & ~(uintptr_t(pageSize) - 1));
    g_watch.target = host;
    g_watch.guestTarget = guestAddress;

    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&WatchArm), &info))
        g_watch.moduleBase = uintptr_t(info.dli_fbase);

    struct sigaction sa{};
    sa.sa_sigaction = &OnSegv;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_watch.oldSegv);

    struct sigaction st{};
    st.sa_sigaction = &OnTrap;
    st.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&st.sa_mask);
    sigaction(SIGTRAP, &st, &g_watch.oldTrap);

    g_watch.armed.store(true, std::memory_order_relaxed);
    WatchProtect(PROT_READ);
    lucent::info("hle", "queue watchpoint armed on guest {:#x} (host {}, module base {:#x})",
        guestAddress, static_cast<void*>(host), g_watch.moduleBase);
}

void WatchReport()
{
    if (g_watch.page == nullptr)
        return;
    lucent::Line line;
    line.add("queue watch guest {:#x}: {} target writes, {} other faults on the page",
        g_watch.guestTarget, g_watch.hits.load(), g_watch.otherFaults.load());
    for (const auto& s : g_watch.slots)
    {
        if (s.rip == 0) break;
        line.add("  rip {:#x} (+{:#x}) x{}", s.rip, s.rip - g_watch.moduleBase, s.count);
    }
    line.flush_debug("hle");

    // Re-arm for the next frame: the interesting phase is the 3D scene, which
    // starts hundreds of frames in, so a one-shot sample taken at the first
    // replay would only characterise the intro. Samples are per-frame deltas.
    for (auto& s : g_watch.slots)
        s = WatchSlot{};
    g_watch.hits.store(0, std::memory_order_relaxed);
    g_watch.otherFaults.store(0, std::memory_order_relaxed);
    g_watch.armed.store(true, std::memory_order_relaxed);
    WatchProtect(PROT_READ);
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_8223B5E0);
namespace {
Probe g_probe_worker{"8223B5E0/queue", 0x8223B5E0};
}
PPC_FUNC(sub_8223B5E0)
{
    // r3 is the worker object; *(r3) is the interpreter context, whose +0x58
    // holds the queued list and +0x50 the resume pointer.
    const uint32_t ctxAddress = ReadGuestBE32(ctx.r3.u32);
    const uint32_t head = ctxAddress ? ReadGuestBE32(ctxAddress + 0x58) : 0;
    if (head == 0)
        ++g_emptyRuns;
    else if (head == g_lastQueueHead)
        ++g_repeatRuns;
    else
        ++g_distinctRuns;
    if (head != 0)
        g_lastQueueHead = head;
    if (ctxAddress)
        WatchArm(ctxAddress + 0x58);
    g_currentList = head;
    g_queueContext = ctxAddress;
    const uint32_t resume = ctxAddress ? ReadGuestBE32(ctxAddress + 0x50) : 0;
    if (head != 0)
        CountReplay(head, resume);
    Note(g_probe_worker, uint32_t(ctx.lr));
    __imp__sub_8223B5E0(ctx, base);
    if (ctxAddress && head != 0)
    {
        if (ReadGuestBE32(ctxAddress + 0x50) != 0)
            ++g_replaySuspended;
        else
            ++g_replayCompleted;
    }
}

// The ring kick. Censuses the PM4 header of every packet the CPU command-list
// interpreter appends, so the six packets each replay emits can be named -- in
// particular how many of them are the INTERRUPT that re-enqueues the list.
namespace {
Probe g_probe_kick{"822212D8", 0x822212D8};
struct RegKick { RegKick() { Register(&g_probe_kick); } } g_regKick;
}
extern "C" PPC_FUNC(__imp__sub_822212D8);
PPC_FUNC(sub_822212D8)
{
    ++g_kicks;
    CountValue(g_headers, kValueSlots, ReadGuestBE32(ctx.r4.u32));
    Note(g_probe_kick, uint32_t(ctx.lr));
    __imp__sub_822212D8(ctx, base);
}

// The GPU interrupt callback the command stream nominates in SCRATCH_REG4:
//   stw r3, 0x2A94(pool)   ; = <interpreter ctx>+0x58, the worker's queue
//   KeSetEvent(&percpu_event[cpu])
// r3 is the list pointer the submission carried in SCRATCH_REG5.
extern "C" PPC_FUNC(__imp__sub_8223B8A0);
PPC_FUNC(sub_8223B8A0)
{
    const uint32_t list = ctx.r3.u32;
    ++g_enqueues;
    if (list == g_currentList && list != 0)
        ++g_enqueueSelf;
    if (g_queueContext && ReadGuestBE32(g_queueContext + 0x58) != 0)
        ++g_enqueueOverwrites;
    CountValue(g_enqueued, kValueSlots, list);
    __imp__sub_8223B8A0(ctx, base);
}

// ---------------------------------------------------------------------------
// Shared helpers for the shader instrumentation below, plus the two census
// probes on the D3D draw path.
//
// Reads of candidate guest pointers go through process_vm_readv so an unmapped
// guest page returns EFAULT instead of faulting the process: an argument
// register is full of values that look like pointers and are not.
// ---------------------------------------------------------------------------

#include <sys/syscall.h>
#include <sys/uio.h>

#include <mutex>

namespace
{

// Defined with the argument-scan probes at the end of this file.
void ArgReport();
void ArgScanInit();

bool GuestCopy(uint32_t guest, void* out, size_t n)
{
    iovec local{out, n};
    iovec remote{gears::Memory().Translate<uint8_t>(guest), n};
    long r = syscall(SYS_process_vm_readv, getpid(), &local, 1UL, &remote, 1UL, 0UL);
    return r == long(n);
}

bool IsShaderContainerWord(uint32_t w) { return (w & 0xFFFFFF00u) == 0x102A1100u; }

// A guest address worth spending a syscall on. Anything else in a register is
// a float, a bitfield or a small integer.
bool PlausiblePointer(uint32_t v)
{
    if (v & 3)
        return false;
    return (v >= 0x40000000u && v < 0x50000000u) || // title heap
           (v >= 0x80000000u && v < 0x83000000u) || // image
           (v >= 0xA0000000u && v < 0xC0000000u);   // physical windows
}

} // namespace

namespace gears
{

void HleShaderCaptureFrame(uint64_t frame)
{
    ArgScanInit();
    if (frame % 60 == 0)
        ArgReport();
}

} // namespace gears

// SetTexture, and the state-flush-and-draw emitter. Census only: both are on
// the path the shader work has to understand, and their per-frame call counts
// with call-site provenance are what say which phase is doing what.
extern "C" PPC_FUNC(__imp__sub_82220858);
namespace {
Probe g_probe_settex{"82220858/SetTexture", 0x82220858};
struct RegSetTex { RegSetTex() { Register(&g_probe_settex); } } g_regSetTex;
}
PPC_FUNC(sub_82220858)
{
    Note(g_probe_settex, uint32_t(ctx.lr));
    __imp__sub_82220858(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82544148);
namespace {
Probe g_probe_draw{"82544148/draw", 0x82544148};
struct RegDraw { RegDraw() { Register(&g_probe_draw); } } g_regDraw;
}
PPC_FUNC(sub_82544148)
{
    Note(g_probe_draw, uint32_t(ctx.lr));
    __imp__sub_82544148(ctx, base);
}

// ---------------------------------------------------------------------------
// Finding the shader-set entry point.
//
// Nothing in the image names it. So this asks the question directly at the API
// boundary: a D3D shader object contains the container word 0x102A11tt (at
// +0x28 in a pixel-shader object, +0x368 in a vertex-shader one -- it is NOT at
// offset 0, and a detector that assumes so finds nothing), so the function that
// BINDS a shader is a D3D function handed one in an argument register.
// Every D3D-range function
// reachable from the UE3 RHI zone (0x82540000-0x82560000, 51 of them by a
// static bl-scan) gets the same argument probe; the ones that never receive a
// shader report zero and are eliminated by measurement rather than by guesswork.
//
// Result (catalog #21): sub_82222808 is SetPixelShader and sub_82222B98 is
// SetVertexShader, r3 = device, r4 = shader.
//
// Enabled with GEARS_SHADER_ARGSCAN=1.
// ---------------------------------------------------------------------------

namespace
{

bool g_argScan = false;

struct ArgProbe
{
    const char* name;
    uint64_t calls = 0;
    uint64_t hits[8]{};       // per argument register r3..r10
    uint32_t types[8]{};      // bitmask of shader types seen in that register
    uint32_t lastShader[8]{};
    uint32_t distinct[8]{};   // distinct shader pointers, capped
    uint32_t magicOffset[8]{};// where the container word sat inside the object
    uint32_t seen[8][16]{};
};

constexpr size_t kMaxArgProbes = 96;
ArgProbe* g_argProbes[kMaxArgProbes]{};
size_t g_argProbeCount = 0;
std::mutex g_argLock;

void RegisterArg(ArgProbe* p)
{
    if (g_argProbeCount < kMaxArgProbes)
        g_argProbes[g_argProbeCount++] = p;
}

void ArgScan(ArgProbe& p, const PPCContext& ctx)
{
    const uint32_t regs[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                              ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
    std::lock_guard<std::mutex> guard(g_argLock);
    ++p.calls;
    for (int i = 0; i < 8; ++i)
    {
        const uint32_t v = regs[i];
        if (!PlausiblePointer(v))
            continue;
        // The D3D shader object is NOT the bare container: the setters found by
        // this scan are handed objects with fields well past 0x300, so the
        // container word can sit at an offset. Search a bounded prefix and
        // record where it was found rather than requiring it at +0.
        uint32_t head[256];
        if (!GuestCopy(v, head, sizeof(head)))
            continue;
        uint32_t first = 0;
        int at = -1;
        for (int w = 0; w < 256; ++w)
        {
            const uint32_t x = __builtin_bswap32(head[w]);
            if (IsShaderContainerWord(x)) { first = x; at = w; break; }
        }
        if (at < 0)
            continue;
        p.magicOffset[i] = uint32_t(at) * 4;
        ++p.hits[i];
        p.types[i] |= 1u << (first & 0xFF);
        p.lastShader[i] = v;
        bool known = false;
        for (uint32_t k = 0; k < p.distinct[i] && k < 16; ++k)
            if (p.seen[i][k] == v) { known = true; break; }
        if (!known)
        {
            if (p.distinct[i] < 16)
                p.seen[i][p.distinct[i]] = v;
            ++p.distinct[i];
        }
    }
}

void ArgScanInit()
{
    g_argScan = lucent::config::flag("SHADER_ARGSCAN");
    if (g_argScan)
        lucent::info("hle", "shader argument scan armed over {} D3D entry points",
            g_argProbeCount);
}

void ArgReport()
{
    if (!g_argScan)
        return;
    std::lock_guard<std::mutex> guard(g_argLock);
    for (size_t i = 0; i < g_argProbeCount; ++i)
    {
        ArgProbe* p = g_argProbes[i];
        // Probes with zero shader arguments are reported too: "called 200000
        // times and never handed a shader" is the measurement that eliminates a
        // candidate, and silence would not distinguish it from "never called".
        if (p->calls == 0)
            continue;
        lucent::Line line;
        line.add("argscan sub_{} calls={}", p->name, p->calls);
        for (int r = 0; r < 8; ++r)
        {
            if (!p->hits[r])
                continue;
            line.add("  r{}: {} shader args, {} distinct, types{}{}, container word at +{:#x}, last {:#x}", r + 3,
                p->hits[r], p->distinct[r],
                (p->types[r] & 1) ? " ps" : "", (p->types[r] & 2) ? " vs" : "",
                p->magicOffset[r], p->lastShader[r]);
        }
        line.flush_debug("hle");
    }
}

} // namespace

#define GEARS_HLE_ARGPROBE(addr)                                               \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                    \
    namespace {                                                                \
    ArgProbe g_arg_##addr{#addr};                                              \
    struct RegArg_##addr { RegArg_##addr() { RegisterArg(&g_arg_##addr); } }   \
        g_regArg_##addr;                                                       \
    }                                                                          \
    PPC_FUNC(sub_##addr)                                                       \
    {                                                                          \
        if (g_argScan)                                                         \
            ArgScan(g_arg_##addr, ctx);                                        \
        __imp__sub_##addr(ctx, base);                                          \
    }

// The 51 D3D-range functions called from the UE3 RHI zone, minus the ones
// already overridden above (0x82220858, 0x82221980) and 0x8221CBA8, which the
// seam notes identify as UE3's render-command ring allocator, not D3D.
GEARS_HLE_ARGPROBE(8222E8E0)
GEARS_HLE_ARGPROBE(82222350)
GEARS_HLE_ARGPROBE(8221D9B8)
GEARS_HLE_ARGPROBE(82222460)
GEARS_HLE_ARGPROBE(8222E868)
GEARS_HLE_ARGPROBE(8222E758)
GEARS_HLE_ARGPROBE(8222ABF8)
GEARS_HLE_ARGPROBE(8222CC48)
GEARS_HLE_ARGPROBE(82218928)
GEARS_HLE_ARGPROBE(8222B068)
GEARS_HLE_ARGPROBE(8222AE20)
GEARS_HLE_ARGPROBE(8222B398)
GEARS_HLE_ARGPROBE(82222808)
GEARS_HLE_ARGPROBE(82222B98)
GEARS_HLE_ARGPROBE(8222AFD8)
GEARS_HLE_ARGPROBE(8222A2D8)
GEARS_HLE_ARGPROBE(8222A150)
GEARS_HLE_ARGPROBE(82235528)
GEARS_HLE_ARGPROBE(8222CFF8)
GEARS_HLE_ARGPROBE(82220570)
GEARS_HLE_ARGPROBE(82222E18)
GEARS_HLE_ARGPROBE(82222710)
GEARS_HLE_ARGPROBE(82228F70)
GEARS_HLE_ARGPROBE(82229460)
GEARS_HLE_ARGPROBE(8222AB30)
GEARS_HLE_ARGPROBE(8222ECC0)
GEARS_HLE_ARGPROBE(82222AC8)
GEARS_HLE_ARGPROBE(82220028)
GEARS_HLE_ARGPROBE(8221F8B0)
GEARS_HLE_ARGPROBE(8222DE50)
GEARS_HLE_ARGPROBE(82236370)
GEARS_HLE_ARGPROBE(82228998)
GEARS_HLE_ARGPROBE(82228AB8)
GEARS_HLE_ARGPROBE(82228B48)
GEARS_HLE_ARGPROBE(82229398)
GEARS_HLE_ARGPROBE(8222D4F8)
GEARS_HLE_ARGPROBE(82229028)
GEARS_HLE_ARGPROBE(82222EF8)
GEARS_HLE_ARGPROBE(82229B28)
GEARS_HLE_ARGPROBE(82228A28)
GEARS_HLE_ARGPROBE(82228D28)
GEARS_HLE_ARGPROBE(82228BD8)
GEARS_HLE_ARGPROBE(82228C48)
GEARS_HLE_ARGPROBE(82228CB8)
GEARS_HLE_ARGPROBE(822364F8)
GEARS_HLE_ARGPROBE(8222EF20)
GEARS_HLE_ARGPROBE(822369F0)
GEARS_HLE_ARGPROBE(82221D78)
