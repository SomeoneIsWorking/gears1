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
#include <lucent/log.h>

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
GEARS_HLE_TRACE(822212D8) // ring kick: append IB packet, store CP_RB_WPTR
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

} // namespace

namespace gears
{

void HleWorkerCensus()
{
    lucent::debug("hle", "worker replays: {} with a new list, {} repeating the "
                         "previous list, {} with an empty queue",
        g_distinctRuns, g_repeatRuns, g_emptyRuns);
}

} // namespace gears

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
    Note(g_probe_worker, uint32_t(ctx.lr));
    __imp__sub_8223B5E0(ctx, base);
}
