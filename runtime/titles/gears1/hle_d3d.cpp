// Gears 1 overrides of guest D3D functions, and the instrumentation used to
// derive their contract.
//
// Mechanism: XenonRecomp emits every translated function as a retained
// `__imp__sub_X` implementation plus a weak noinline `sub_X` forwarder. A
// strong definition of `sub_X` in this translation unit therefore replaces
// the guest body at every direct call site, while `__imp__sub_X` stays
// reachable as a super-call. Nothing here goes through PPCFuncMappings: that
// table only serves indirect calls, and the D3D layer is called with plain
// `bl`.
#include "import_stub.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_memory.h"
#include "guest_write_watch.h"
#include "hle_d3d.h"
#include "gpu_shader_load_watch.h"
#include "shader_flush_capture.h"

// ---------------------------------------------------------------------------
// Registration helper. GEARS_HLE_TRACE(addr) defines a strong sub_<addr> that
// counts the call, records the return address (= the call site, which is the
// only caller provenance the translated code carries), and chains to the guest
// body.
// ---------------------------------------------------------------------------

namespace
{

[[nodiscard]] bool HleCensusEnabled()
{
    static const bool enabled = lucent::config::flag("HLE_CENSUS");
    return enabled;
}

[[nodiscard]] bool QueueWatchEnabled()
{
    static const bool enabled = lucent::config::flag("WATCH_QUEUE");
    return enabled;
}

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
    const char *name;
    uint32_t address;
    std::atomic<uint64_t> calls{0};
    CallSite sites[8]{};
    // Calls from a NINTH distinct return address. They are in `calls` but their
    // site is gone, so the site list must not be read as the complete set of
    // callers -- which is exactly what it looks like without this.
    std::atomic<uint64_t> sitesDropped{0};
};

constexpr size_t kMaxProbes = 32;
Probe *g_probes[kMaxProbes]{};
std::atomic<size_t> g_probeCount{0};

void Register(Probe *p)
{
    size_t i = g_probeCount.fetch_add(1);
    if (i < kMaxProbes)
        g_probes[i] = p;
    // Over the cap the probe still instruments the guest function -- it just
    // can never appear in the census. Its absence would read as "that function
    // is never called". See the census, which refuses to print a clean list
    // while any probe is unreportable.
}

void Note(Probe &p, uint32_t lr)
{
    p.calls.fetch_add(1, std::memory_order_relaxed);
    for (auto &s : p.sites)
    {
        if (s.lr == lr)
        {
            ++s.count;
            return;
        }
        if (s.lr == 0)
        {
            s.lr = lr;
            s.count = 1;
            return;
        }
    }
    p.sitesDropped.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

namespace gears::titles::gears1
{

void DumpHleD3dCensus(const char *why)
{
    if (!HleCensusEnabled())
        return;
    size_t n = g_probeCount.load();
    static bool announced = false;
    if (!announced)
    {
        announced = true;
        lucent::info("hle", "{} probes registered", n);
        // The census can only ever walk the first kMaxProbes slots. A probe past
        // the cap instruments its guest function and then reports nothing, so
        // "sub_X never appears in the census" would read as "sub_X is never
        // called". Loud, once, because it invalidates every absence below it.
        if (n > kMaxProbes)
            lucent::error("hle",
                          "{} probes registered but only {} fit: {} probe(s)"
                          " CANNOT APPEAR IN THIS CENSUS AT ALL. Raise kMaxProbes -- until"
                          " then a function missing from the census below may simply be"
                          " one of the unreportable ones",
                          n, kMaxProbes, n - kMaxProbes);
    }
    for (size_t i = 0; i < n && i < kMaxProbes; ++i)
    {
        Probe *p = g_probes[i];
        if (!p || p->calls.load() == 0)
            continue;
        lucent::Line line;
        line.add("{} sub_{} calls={}", why, p->name, p->calls.load());
        for (const auto &s : p->sites)
        {
            if (s.lr == 0)
                break;
            line.add("  from {:#x} x{}", s.lr, s.count);
        }
        // Without this the eight-slot table reads as the complete caller set.
        if (const uint64_t dropped = p->sitesDropped.load())
            line.add("  AND {} call(s) from further sites that did not fit in the"
                     " {}-slot table -- this caller list is INCOMPLETE",
                     dropped, sizeof(p->sites) / sizeof(p->sites[0]));
        line.flush_debug("hle");
    }
}

} // namespace gears::titles::gears1

#define GEARS_HLE_TRACE(addr)                                                                      \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                        \
    namespace                                                                                      \
    {                                                                                              \
    Probe g_probe_##addr{#addr, 0x##addr};                                                         \
    struct Reg_##addr                                                                              \
    {                                                                                              \
        Reg_##addr() { Register(&g_probe_##addr); }                                                \
    } g_reg_##addr;                                                                                \
    }                                                                                              \
    PPC_FUNC(sub_##addr)                                                                           \
    {                                                                                              \
        if (HleCensusEnabled())                                                                    \
            Note(g_probe_##addr, uint32_t(ctx.lr));                                                \
        __imp__sub_##addr(ctx, base);                                                              \
    }

// CANDIDATE DRAW EMITTERS. The frame's draws are emitted ~744 times a frame in
// gameplay and ~170 in menus (the renderer counts them independently), so a
// per-frame rate identifies the emitter on its own -- which is how 0x82544148 was
// ruled out at exactly 1 per frame (catalog #58). 0x8221D3A8 is the path the movie
// phase draws through, per the seam map.
GEARS_HLE_TRACE(8221D3A8) // movie-phase draw path

// The submission chain, bottom to top.
extern "C" PPC_FUNC(__imp__sub_822218C0);
namespace
{
Probe g_probe_822218C0{"822218C0", 0x822218C0};
struct Reg_822218C0
{
    Reg_822218C0() { Register(&g_probe_822218C0); }
} g_reg_822218C0;
} // namespace
PPC_FUNC(sub_822218C0)
{
    const bool shaderPacketWatch = gears::ShaderLoadPacketWatchEnabled();
    const std::uint64_t targetWritesBefore =
        shaderPacketWatch
            ? gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kShaderLoadPacket)
                  .targetWrites
            : 0;
    const std::uint32_t caller = static_cast<std::uint32_t>(ctx.lr);
    if (HleCensusEnabled())
        Note(g_probe_822218C0, caller);
    __imp__sub_822218C0(ctx, base);
    if (shaderPacketWatch)
    {
        const std::uint64_t targetWritesAfter =
            gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kShaderLoadPacket)
                .targetWrites;
        if (targetWritesAfter > targetWritesBefore)
        {
            lucent::info("hle", "shader-load selected direct submission helper caller {:#x}",
                         caller);
        }
    }
}
extern "C" PPC_FUNC(__imp__sub_82221980);
namespace
{
Probe g_probe_82221980{"82221980", 0x82221980};
struct Reg_82221980
{
    Reg_82221980() { Register(&g_probe_82221980); }
} g_reg_82221980;
} // namespace
PPC_FUNC(sub_82221980)
{
    if (HleCensusEnabled())
        Note(g_probe_82221980, uint32_t(ctx.lr));
    if (!gears::titles::gears1::ShaderFlushCaptureActive())
    {
        __imp__sub_82221980(ctx, base);
        return;
    }
    const std::uint32_t device = ctx.r3.u32;
    const std::uint32_t oldCommandEnd = ReadGuestBE32(device + 0x28);
    __imp__sub_82221980(ctx, base);
    gears::titles::gears1::ObserveShaderFlushCommandBufferTransition(device, oldCommandEnd,
                                                                     ReadGuestBE32(device + 0x28));
}
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
    for (auto &s : g_replays)
    {
        if (s.count && s.list == list && s.resume == resume)
        {
            ++s.count;
            return;
        }
        if (s.count == 0)
        {
            s.list = list;
            s.resume = resume;
            s.count = 1;
            return;
        }
    }
    ++g_replayOverflow;
}

void CountValue(ValueSlot *slots, size_t n, uint32_t value)
{
    for (size_t i = 0; i < n; ++i)
    {
        if (slots[i].value == value)
        {
            ++slots[i].count;
            return;
        }
        if (slots[i].count == 0)
        {
            slots[i].value = value;
            slots[i].count = 1;
            return;
        }
    }
}

} // namespace

namespace gears::titles::gears1
{

void ReportHleD3dWorkerCensus()
{
    if (!HleCensusEnabled())
    {
        if (QueueWatchEnabled())
            ReportGuestWriteWatch(GuestWriteWatchOwner::kQueue, true);
        return;
    }
    lucent::debug("hle",
                  "worker replays: {} with a new list, {} repeating the "
                  "previous list, {} with an empty queue",
                  g_distinctRuns, g_repeatRuns, g_emptyRuns);
    {
        lucent::Line line;
        for (const auto &s : g_enqueued)
        {
            if (s.count == 0)
                break;
            ++g_enqueueDistinct;
        }
        line.add("queue enqueues by sub_8223B8A0 this frame: {} total, {} distinct lists, {} "
                 "overwrote an unconsumed list, {} re-enqueued the list being replayed",
                 g_enqueues, g_enqueueDistinct, g_enqueueOverwrites, g_enqueueSelf);
        size_t shown = 0;
        for (const auto &s : g_enqueued)
        {
            if (s.count == 0)
                break;
            if (shown++ < 16)
                line.add("  list {:#x} x{}", s.value, s.count);
        }
        line.flush_debug("hle");
        // Per-frame deltas: clear so the next census describes one frame.
        for (auto &s : g_enqueued)
            s = ValueSlot{};
        g_enqueues = g_enqueueOverwrites = g_enqueueSelf = g_enqueueDistinct = 0;
    }
    {
        lucent::Line line;
        line.add("ring kicks this frame: {}", g_kicks);
        size_t shown = 0;
        for (const auto &s : g_headers)
        {
            if (s.count == 0)
                break;
            if (shown++ < 16)
                line.add("  IB size {} x{}", s.value, s.count);
        }
        line.flush_debug("hle");
        for (auto &s : g_headers)
            s = ValueSlot{};
        g_kicks = 0;
    }
    {
        lucent::Line line;
        size_t distinct = 0, maxCount = 0;
        for (const auto &s : g_replays)
        {
            if (s.count == 0)
                break;
            ++distinct;
            if (s.count > maxCount)
                maxCount = s.count;
        }
        line.add("replay progress this frame: {} replays over {} distinct (list,resume) "
                 "points, worst point repeated {}x, {} overflowed the table; {} suspended, "
                 "{} ran to completion",
                 g_replayTotal, distinct, maxCount, g_replayOverflow, g_replaySuspended,
                 g_replayCompleted);
        size_t shown = 0;
        for (const auto &s : g_replays)
        {
            if (s.count == 0)
                break;
            if (shown++ < 12)
                line.add("  list {:#x} resume {:#x} x{}", s.list, s.resume, s.count);
        }
        line.flush_debug("hle");
        for (auto &s : g_replays)
            s = ReplaySlot{};
        g_replayTotal = g_replayOverflow = g_replaySuspended = g_replayCompleted = 0;
    }
    ReportGuestWriteWatch(GuestWriteWatchOwner::kQueue, true);
}

} // namespace gears::titles::gears1

// ---------------------------------------------------------------------------
// The D3D worker queue observation point. guest_write_watch owns the shared
// signal/mprotect machinery; this wrapper owns only discovery of the exact
// queue field from the running worker object.
// ---------------------------------------------------------------------------

namespace
{

constexpr size_t kWatchSamples = 64;

void WatchArm(uint32_t guestAddress)
{
    static bool tried = false;
    if (tried)
        return;
    tried = true;
    if (!QueueWatchEnabled())
        return;
    gears::ArmGuestWriteWatch(gears::GuestWriteWatchOwner::kQueue, guestAddress, kWatchSamples);
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_8223B5E0);
namespace
{
Probe g_probe_worker{"8223B5E0/queue", 0x8223B5E0};
}
PPC_FUNC(sub_8223B5E0)
{
    if (!HleCensusEnabled() && !QueueWatchEnabled())
    {
        __imp__sub_8223B5E0(ctx, base);
        return;
    }
    // r3 is the worker object; *(r3) is the interpreter context, whose +0x58
    // holds the queued list and +0x50 the resume pointer.
    const uint32_t ctxAddress = ReadGuestBE32(ctx.r3.u32);
    if (!HleCensusEnabled())
    {
        if (ctxAddress)
            WatchArm(ctxAddress + 0x58);
        __imp__sub_8223B5E0(ctx, base);
        return;
    }
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
namespace
{
Probe g_probe_kick{"822212D8", 0x822212D8};
struct RegKick
{
    RegKick() { Register(&g_probe_kick); }
} g_regKick;
} // namespace
extern "C" PPC_FUNC(__imp__sub_822212D8);
PPC_FUNC(sub_822212D8)
{
    const bool shaderPacketWatch = gears::ShaderLoadPacketWatchEnabled();
    const std::uint32_t payload = ctx.r4.u32;
    const std::uint64_t targetWritesBefore =
        shaderPacketWatch
            ? gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kShaderLoadPacket)
                  .targetWrites
            : 0;
    if (HleCensusEnabled())
    {
        ++g_kicks;
        CountValue(g_headers, kValueSlots, ReadGuestBE32(ctx.r4.u32));
        Note(g_probe_kick, uint32_t(ctx.lr));
    }
    __imp__sub_822212D8(ctx, base);
    if (shaderPacketWatch)
    {
        const std::uint64_t targetWritesAfter =
            gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kShaderLoadPacket)
                .targetWrites;
        if (targetWritesAfter > targetWritesBefore)
        {
            lucent::info("hle",
                         "shader-load selected ring submission payload: {} dword(s) at guest "
                         "{:#x}, writer callsite {:#x}",
                         ReadGuestBE32(payload), ReadGuestBE32(payload + 4),
                         static_cast<std::uint32_t>(ctx.lr));
        }
    }
}

// The GPU interrupt callback the command stream nominates in SCRATCH_REG4:
//   stw r3, 0x2A94(pool)   ; = <interpreter ctx>+0x58, the worker's queue
//   KeSetEvent(&percpu_event[cpu])
// r3 is the list pointer the submission carried in SCRATCH_REG5.
extern "C" PPC_FUNC(__imp__sub_8223B8A0);
PPC_FUNC(sub_8223B8A0)
{
    if (!HleCensusEnabled())
    {
        __imp__sub_8223B8A0(ctx, base);
        return;
    }
    const uint32_t list = ctx.r3.u32;
    ++g_enqueues;
    if (list == g_currentList && list != 0)
        ++g_enqueueSelf;
    if (g_queueContext && ReadGuestBE32(g_queueContext + 0x58) != 0)
        ++g_enqueueOverwrites;
    CountValue(g_enqueued, kValueSlots, list);
    __imp__sub_8223B8A0(ctx, base);
}

namespace gears::titles::gears1
{
namespace
{

struct HleD3dDiagnosticsRegistration
{
    HleD3dDiagnosticsRegistration()
    {
        if (!gears::InstallHleD3dDiagnostics(
                {.dumpCensus = DumpHleD3dCensus, .workerCensus = ReportHleD3dWorkerCensus}))
            std::abort();
    }
};

HleD3dDiagnosticsRegistration g_hleD3dDiagnosticsRegistration;

} // namespace
} // namespace gears::titles::gears1
