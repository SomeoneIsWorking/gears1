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
