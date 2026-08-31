#include "guest_probe_state.h"
#include "import_stub.h"

#include <atomic>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <byteswap.h>
#include <lucent/config.h>
#include <lucent/log.h>

#include "frame_production_timing.h"
#include "guest_backtrace.h"
#include "guest_memory.h"
#include "guest_thread.h"
#include "gpu_shader_load_watch.h"

// ---------------------------------------------------------------------------
// The render-command ring buffer (catalog #44).
//
// sub_82444EF0 is UE3's RenderingThreadMain draining an FRingBuffer at
// 0x82C0CB24, and sub_8221CBA8 is the allocator that reserves space in it.
// FRingBuffer assumes exactly ONE producer and ONE consumer, and its drain loop
// advances ReadPointer by whatever the command's Execute() returns -- so once
// the read cursor is off a command boundary it never recovers and the next
// "vtable" it reads is payload bytes.
//
// These probes answer the three questions that discriminate the causes, and
// they answer them by MEASUREMENT rather than by reading the code again:
//   1. does more than one thread enter either path?
//   2. what was the last command that executed cleanly before the divergence?
//   3. does the bad object pointer lie inside the ring's own Data..DataEnd?
extern "C" PPC_FUNC(__imp__sub_82444EF0);
extern "C" PPC_FUNC(__imp__sub_8221CBA8);

namespace
{
constexpr uint32_t kRenderRing = 0x82C0CB24;

std::atomic<uint32_t> g_drainThreads{0};
std::atomic<uint32_t> g_producerThreads{0};

// Track each OS thread identity once. Guest work legitimately alternates
// between named roles, so repeatedly comparing only with the immediately
// previous entrant turns ordinary alternation into an unbounded error stream.
std::mutex g_ringMutex;
std::set<long> g_drainThreadIds;
std::set<long> g_producerThreadIds;
std::string g_firstDrainThreadName;
std::string g_firstProducerThreadName;

struct RenderRingReservation
{
    uint32_t bytes = 0;
    uint32_t caller = 0;
};

std::map<uint32_t, RenderRingReservation> g_ringReservations;

void NoteRingThread(const char *which, std::set<long> &knownThreadIds, std::string &firstThreadName,
                    std::atomic<uint32_t> &count)
{
    const char *name = gears::GuestThreadName();
    // THE TID IS PART OF THE IDENTITY, not decoration. 'host' is the DEFAULT
    // name for every thread that has never run guest code (guest_thread.cpp),
    // so two different host threads entering the producer compared EQUAL and
    // the detector stayed silent about them -- which is exactly catalog #44's
    // open question, WHICH host thread is entering the guest's render ring.
    // Appending the tid makes the comparison distinguish them as well as
    // naming them.
    const long threadId = long(syscall(SYS_gettid));
    const std::string current =
        std::string(name ? name : "?") + " (tid " + std::to_string(threadId) + ")";
    std::lock_guard<std::mutex> guard(g_ringMutex);
    if (!knownThreadIds.insert(threadId).second)
        return;
    if (knownThreadIds.size() == 1)
    {
        firstThreadName = current;
        lucent::info("ring", "{} entered by guest thread '{}'", which, current);
        return;
    }

    // A distinct identity is evidence that the path is not permanently owned
    // by one host thread. It is not by itself evidence of concurrent entry;
    // RingProducerOverlaps is the instrument that answers that question.
    lucent::error("ring",
                  "{} entered by another distinct thread: '{}' after"
                  " first entrant '{}' ({} distinct thread(s), producer entries so far:"
                  " {}, overlaps caught by the wait: {})",
                  which, current, firstThreadName, knownThreadIds.size(),
                  gears::titles::gears1::RingProducerEntries(),
                  gears::titles::gears1::RingProducerOverlaps());
    count.fetch_add(1);
}
} // namespace

PPC_FUNC(sub_8221CBA8)
{
    // CHECK WHICH RING. The allocator takes the ring in r4, and this probe used
    // to ignore it -- so every allocation from any ring counted as a render-ring
    // producer. It happens not to matter (all 45 call sites in this image target
    // 0x82C0CB24, so there is exactly one such ring), but an instrument that
    // assumes its own premise is how the last several wrong conclusions
    // happened. Checking costs one compare.
    if (ctx.r4.u32 != kRenderRing)
    {
        __imp__sub_8221CBA8(ctx, base);
        return;
    }

    // A GUARD THAT HAS NEVER NEEDED TO FIRE -- and the count is the point.
    //
    // MEASURED: 137,561 producer entries in one run, ZERO overlaps. The two
    // producers (game thread and rendering thread, ~30 alternations a run) never
    // overlap the reserve/commit window. So the lost-update hypothesis for
    // catalog #44 is NOT supported, and this wait is inert in practice. It is
    // kept as a DETECTOR rather than presented as a fix: if an overlap ever does
    // happen it is both prevented and reported, and until then the zero carries
    // its denominator so nobody has to re-derive it.
    //
    // HONOURING THE TITLE'S OWN PRODUCER FLAG.
    //
    // The ring's +16 (bIsWriting) is set to 1 by this allocator and cleared by
    // the caller's commit. It is a producer lock -- and image-wide, NOTHING EVER
    // READS IT (all 43 sites materialising the ring were scanned; the three
    // candidate loads at +16 were all off a different register). On hardware it
    // did not need to be read: the commit is three instructions, so two
    // producers essentially never interleaved.
    //
    // Recompiled, that window is tens of host instructions with volatile
    // accesses. The game thread and rendering thread alternate entries here,
    // but distinct identities do not establish simultaneous access. The open
    // question is whether their reserve/commit intervals overlap, which is what
    // this wait measures.
    //
    // So this waits for the flag the title already maintains. It is not a lock
    // invented here; it completes a protocol the title defines and hardware made
    // unnecessary. Bounded, because a producer that reserves and never commits
    // would otherwise hang the process -- and if that bound is ever hit, it is
    // reported rather than silently ignored.
    {
        const uint32_t writingFlag = kRenderRing + 16;
        constexpr int kMaxSpins = 20000;
        int spins = 0;
        while (ByteSwap(*gears::Memory().Translate<uint32_t>(writingFlag)) != 0)
        {
            if (++spins > kMaxSpins)
            {
                lucent::error("ring",
                              "bIsWriting stayed set for {} spins --"
                              " proceeding anyway. Either a producer reserved without"
                              " committing, or this wait is wrong; it must not hang the"
                              " process either way",
                              kMaxSpins);
                break;
            }
            std::this_thread::yield();
        }
        // COUNTED AND REPORTED AT INFO, not debug. The first version of this
        // logged at debug level, so with the channel off it printed nothing --
        // and "the wait never engaged" then looked identical to "the report was
        // switched off". A wait that never engages is a MEANINGFUL result here
        // (it would mean the two producers never actually overlap, which would
        // undercut the lost-update hypothesis), so it must not be confusable
        // with silence.
        if (spins != 0)
            gears::titles::gears1::CountRingProducerOverlap();
        static std::atomic<uint64_t> engagements{0};
        if (spins != 0)
            lucent::info("ring",
                         "producer wait ENGAGED: {} spin(s) for another"
                         " producer's commit (engagement {})",
                         spins, engagements.fetch_add(1) + 1);
    }
    NoteRingThread("the render-ring allocator", g_producerThreadIds, g_firstProducerThreadName,
                   g_producerThreads);
    gears::titles::gears1::CountRingProducerEntry();

    // WHICH CALL SITE, per thread. Distinct producer identities are only a
    // lost-update race if their reserve/commit intervals overlap; whether that
    // happens is measured above. The caller identifies why each thread enters.
    // Recorded as a set so a hot path cannot flood the log.
    {
        const char *rawName = gears::GuestThreadName();
        const std::string name = rawName ? rawName : "?";
        const uint32_t from = uint32_t(ctx.lr);
        static std::mutex sitesMutex;
        static std::map<std::string, std::set<uint32_t>> sites;
        std::lock_guard<std::mutex> guard(sitesMutex);
        auto &forThread = sites[name];
        if (forThread.insert(from).second && forThread.size() <= 12)
        {
            lucent::info("ring",
                         "producer '{}' enqueues from {:#x} (distinct"
                         " call site {} for this thread)",
                         name, from, forThread.size());

            // THE ROUTE, for any producer that is not the game thread. A direct
            // call graph over the whole image finds no path from the drain loop
            // to this enqueue, so it runs through one of the three indirect
            // dispatches in sub_82444EF0 -- and only a stack walk can say which.
            //
            // The negative is designed: if the walk yields fewer than three
            // frames it says so, because a short trace and a broken walker look
            // identical, and this seam is exactly where that matters.
            if (name != "host")
            {
                const std::vector<uint32_t> frames =
                    gears::GuestBacktrace(ctx.r1.u32, uint32_t(ctx.lr));
                if (frames.size() < 3)
                    lucent::error("ring",
                                  "  the stack walk yielded only {}"
                                  " frame(s) -- too few to name the route. Treat this as"
                                  " the WALKER failing, not as the route being short",
                                  frames.size());
                else
                    lucent::error("ring", "  route from '{}': {}", name,
                                  gears::FormatGuestBacktrace(ctx.r1.u32, uint32_t(ctx.lr)));
            }
        }
    }

    const uint32_t reservationRecord = ctx.r3.u32;
    const uint32_t reservationCaller = uint32_t(ctx.lr);
    __imp__sub_8221CBA8(ctx, base);
    if (gears::ShaderLoadPacketWatchEnabled())
    {
        const uint32_t start =
            ByteSwap(*gears::Memory().Translate<uint32_t>(reservationRecord + 4));
        const uint32_t bytes =
            ByteSwap(*gears::Memory().Translate<uint32_t>(reservationRecord + 8));
        gears::titles::gears1::NoteRenderRingReservation(start, bytes, reservationCaller);
    }
}

PPC_FUNC(sub_82444EF0)
{
    NoteRingThread("the render-ring drain loop", g_drainThreadIds, g_firstDrainThreadName,
                   g_drainThreads);

    const uint32_t data = ByteSwap(*gears::Memory().Translate<uint32_t>(kRenderRing + 0));
    const uint32_t dataEnd = ByteSwap(*gears::Memory().Translate<uint32_t>(kRenderRing + 4));
    lucent::info("ring",
                 "drain loop starting: Data={:#x} DataEnd={:#x}"
                 " (a bad object pointer inside this range means the read cursor is on a"
                 " command's payload rather than its header)",
                 data, dataEnd);

    __imp__sub_82444EF0(ctx, base);
}

// ---------------------------------------------------------------------------
// The two Kismet ops that decide whether a checkpoint restore has anything to
// restore (catalog #45).
//
// sub_82207CC8 sets the "pending data" flag UNCONDITIONALLY on its first tick,
// with no check that any data exists. sub_82207E98 is the op that parses
// "LoadChapter=%i" and copies object+420 INTO the global carrier at 0x82BFB36C
// -- the carrier the restore later copies back out. Measured: that carrier is
// EMPTY at restore time, so the question is whether this op ever runs.
//
// BOTH are probed, and the pairing is the point: sub_82207CC8 firing while
// sub_82207E98 does not is the difference between "the op ran and found
// nothing" and "the op never ran", and only one of those is our problem.
extern "C" PPC_FUNC(__imp__sub_82207CC8);
extern "C" PPC_FUNC(__imp__sub_82207E98);

namespace
{
constexpr uint32_t kChapterCarrier = 0x82BFB36C;

void ReportCarrier(const char *who)
{
    const uint32_t data = ByteSwap(*gears::Memory().Translate<uint32_t>(kChapterCarrier));
    const uint32_t count = ByteSwap(*gears::Memory().Translate<uint32_t>(kChapterCarrier + 4));
    lucent::info("chapter", "{}: the carrier at {:#x} is {} ({} bytes at {:#x})", who,
                 kChapterCarrier, data == 0 || count == 0 ? "EMPTY" : "populated", count, data);
}
} // namespace

PPC_FUNC(sub_82207CC8)
{
    static std::atomic<uint64_t> seen{0};
    const bool first = seen.fetch_add(1) == 0;
    if (first)
        ReportCarrier("the startup Kismet op (which sets the pending flag) entered");
    __imp__sub_82207CC8(ctx, base);
    if (first)
        ReportCarrier("the startup Kismet op returned");
}

PPC_FUNC(sub_82207E98)
{
    static std::atomic<uint64_t> seen{0};
    const uint64_t n = seen.fetch_add(1);
    if (n < 4)
        ReportCarrier("THE LoadChapter OP RAN -- before");
    __imp__sub_82207E98(ctx, base);
    if (n < 4)
        ReportCarrier("THE LoadChapter OP RAN -- after");
}

// ---------------------------------------------------------------------------
// Lifetime of the archive-derived object the checkpoint restore crashes on
// (catalog #45).
//
// The core file settled what the crash IS: the object at the fault is on a pool
// FREE-LIST (214 nodes, the pool descriptor at 0x41421750 holds it as head), and
// its slot-0 word -- read as a vtable -- is really the free-list's next pointer.
// So this is a use-after-free, not bad data.
//
// sub_8242C098 constructs the class (it installs vtable 0x821047A8 after calling
// the FArchive base constructor), and that vtable's slot 0 is sub_8242C180, the
// destructor. Probing both and remembering the last destruction per address
// turns "the object was freed" into "the object was freed HERE, by this caller".
extern "C" PPC_FUNC(__imp__sub_8242C098);
extern "C" PPC_FUNC(__imp__sub_8242C180);

namespace
{
// Small and fixed: the question is which site destroyed ONE object, so a ring
// of the most recent destructions is enough and cannot grow without bound.
struct Destruction
{
    uint32_t object = 0;
    uint32_t from = 0;
    uint64_t ordinal = 0;
};
constexpr size_t kDestructionSlots = 64;
std::mutex g_lifetimeMutex;
Destruction g_destructions[kDestructionSlots];
// AND THE SAME THING WITHOUT THE RING'S BLIND SPOT. A 64-slot ring answers
// "who destroyed this" only if the destruction is recent; the crashing object's
// might be thousands of destructions back. One entry per address, most recent
// wins, bounded by the number of distinct FArchiveAsync addresses the title
// ever uses -- which is small, because the pool recycles them.
std::unordered_map<uint32_t, Destruction> g_lastDestruction;
size_t g_destructionCursor = 0;
uint64_t g_constructed = 0;
uint64_t g_destroyed = 0;
} // namespace

namespace gears::titles::gears1
{
// Reports whether this object was destroyed, and by whom. Called from the fault
// path, where the answer is the whole question.
void ReportArchiveLifetime(uint32_t object)
{
    std::lock_guard<std::mutex> guard(g_lifetimeMutex);
    // THESE COUNTS CAN BOTH BE ZERO WITHOUT MEANING THE CLASS IS UNUSED. The
    // compiler inlines these constructors and destructors at their call sites
    // -- the render-command analysis found exactly that pattern -- so the
    // out-of-line bodies these probes attach to may never be called even while
    // objects of the class are created and destroyed constantly. Zero here is
    // "this seam saw nothing", not "nothing happened".
    lucent::error("lifetime",
                  "{} constructions and {} destructions seen through"
                  " the out-of-line bodies. If both are zero the compiler has inlined them"
                  " and this seam cannot see the class at all -- that is a blind probe,"
                  " not evidence of absence",
                  g_constructed, g_destroyed);
    const auto it = g_lastDestruction.find(object);
    if (it != g_lastDestruction.end())
    {
        lucent::error("lifetime",
                      "  THE CRASHING OBJECT {:#x} WAS DESTROYED"
                      " (destruction #{} of {}) with lr {:#x} -- use after free, and that"
                      " return address names the caller that deleted it",
                      object, it->second.ordinal, g_destroyed, it->second.from);
        return;
    }
    lucent::error("lifetime",
                  "  object {:#x} appears in NONE of the {}"
                  " destructions this seam recorded ({} distinct addresses), so this class"
                  " never destroyed it -- either it is not an FArchiveAsync at all, or it"
                  " died through a path that does not run this destructor",
                  object, g_destroyed, g_lastDestruction.size());
}
} // namespace gears::titles::gears1

// FArchiveAsync::FArchiveAsync(const TCHAR*). See NoteArchiveAsync at the bottom
// of this file for why ArIsError is the whole question.

PPC_FUNC(sub_8242C098)
{
    const uint32_t self = ctx.r3.u32;
    const uint32_t name = ctx.r4.u32;
    {
        std::lock_guard<std::mutex> guard(g_lifetimeMutex);
        ++g_constructed;
    }
    __imp__sub_8242C098(ctx, base);
    gears::titles::gears1::NoteArchiveAsync(self, name);
}

PPC_FUNC(sub_8242C180)
{
    const uint32_t object = ctx.r3.u32;
    const uint32_t from = uint32_t(ctx.lr);
    {
        std::lock_guard<std::mutex> guard(g_lifetimeMutex);
        Destruction &slot = g_destructions[g_destructionCursor % kDestructionSlots];
        slot.object = object;
        slot.from = from;
        slot.ordinal = g_destroyed;
        ++g_destructionCursor;
        ++g_destroyed;
        g_lastDestruction[object] = slot;
    }
    __imp__sub_8242C180(ctx, base);
}

// WHY THERE IS NO PROBE ON THE FAULTING CALL ITSELF.
//
// The obvious one -- a strong sub_824961D0 that reads the object from r24+1376
// on entry -- IS WRONG AND WAS REMOVED. r24 is set up INSIDE that function, so
// on entry it still holds the caller's value and the read returns nonsense
// (0x470075 on the run that exposed this, with a "vtable" of 0). A probe can
// only see a guest function's arguments and its callee-saved registers AFTER
// the prologue, and there is no seam for mid-function state.
//
// The core file is the right instrument for this one: gdb on the dump gives
// r24, r3 and the memory behind them at the exact fault. That is how the object
// was identified as a pool free-list node in the first place.

// THE POOL'S FREE, identified from the allocator's own vtable at runtime:
// slot +4 is allocate (sub_82214F50), +8 is realloc (sub_822151F8) and
// +12 is sub_822153F0. Probing it is the only seam that can name who releases
// the object the checkpoint restore later calls through -- the class's
// constructor and destructor are inlined, so that seam sees nothing.
extern "C" PPC_FUNC(__imp__sub_822153F0);

namespace
{
// Every free, keyed by address, keeping only the most recent site per address.
// Bounded by the number of distinct addresses the title frees, which is what
// makes this affordable; the value is one word.
std::mutex g_freeMutex;
std::unordered_map<uint32_t, uint32_t> g_lastFreeSite;
std::atomic<uint64_t> g_frees{0};
} // namespace

namespace gears::titles::gears1
{
void ReportLastFree(uint32_t address)
{
    std::lock_guard<std::mutex> guard(g_freeMutex);
    const auto it = g_lastFreeSite.find(address);
    if (it == g_lastFreeSite.end())
    {
        lucent::error("lifetime",
                      "  {:#x} was never freed through the pool"
                      " ({} frees seen). If that seems wrong, check this seam fires at"
                      " all before concluding anything from it",
                      address, g_frees.load());
        return;
    }
    lucent::error("lifetime",
                  "  {:#x} WAS FREED, most recently from {:#x}."
                  " That is the caller to look at",
                  address, it->second);
}
} // namespace gears::titles::gears1

PPC_FUNC(sub_822153F0)
{
    const uint32_t address = ctx.r4.u32;
    const uint32_t from = uint32_t(ctx.lr);
    if (address != 0)
    {
        std::lock_guard<std::mutex> guard(g_freeMutex);
        g_lastFreeSite[address] = from;
    }
    g_frees.fetch_add(1);

    // GEARS_WATCH_FREE=<guest address> reports the moment that address is
    // released, with the caller that did it. The crash this exists for is a
    // use-after-free whose object address is known from the core file, and a
    // raw SIGSEGV leaves no clean exit to dump a table at -- so the report has
    // to happen live, when the free occurs.
    static const uint32_t watched = []
    {
        const uint32_t value = lucent::config::number("WATCH_FREE", 0);
        if (value != 0)
            lucent::info("lifetime", "watching for the release of {:#x}", value);
        return value;
    }();
    gears::titles::gears1::NotePoolEvent(address, true);

    // NO INTERVENTION HERE -- two attempts were made and BOTH FAILED, and the
    // second one is worth not repeating: see ClearCachesOfFreedBlock's comment
    // above for why nulling the field merely moved the crash, and why keeping the
    // block alive did not help either. The reason the second failed is now
    // suspected: the HOLDER is itself a pool block (0x42b40940 on the run that
    // exposed it, in the same 0x42b range as the freed objects), so the dangling
    // thing may be the holder rather than the object it caches -- in which case
    // holder+1376 is being read out of memory that was recycled, and protecting
    // the cached object cannot help. Detection stays; intervention waits for that
    // to be established.
    if (watched != 0 && address == watched)
        lucent::error("lifetime",
                      "WATCHED ADDRESS {:#x} FREED from {:#x}"
                      " (free #{})",
                      address, from, g_frees.load());

    __imp__sub_822153F0(ctx, base);
}

// ---------------------------------------------------------------------------
// Is the checkpoint object still CACHED when the pool frees it? (catalog #45)
//
// sub_824961D0 takes the holder in r3 and immediately does `mr r24,r3`, so the
// holder IS available at entry -- an earlier probe read r24 at entry, which is
// still the CALLER's value, and reported nonsense. The prologue then does
//     lwz r11,1376(r24) ; cmplwi 0 ; bne <use the cache>
// so the cached pointer is used whenever it is merely NON-NULL. Nothing nulls
// it when the object dies, which is the whole defect -- if the object is indeed
// still cached at the moment the pool releases it.
//
// That is what this measures. Every holder seen is remembered, and every free is
// checked against their caches. The negative is designed: if no holder is ever
// seen, the report says the seam never fired rather than implying the cache was
// always clean.

namespace
{
std::mutex g_holderMutex;
std::set<uint32_t> g_holders;
std::atomic<uint64_t> g_holderVisits{0};
} // namespace

namespace gears::titles::gears1
{
// Called from the pool-free probe: does any live holder still cache this block?
// NATIVE OWNERSHIP OF THE CACHE INVARIANT.
//
// This is a PC port, so "the title's code races" is not an end state -- we own
// this behaviour and have to make it correct here.
//
// THE DEFECT: the game thread caches an object at holder+1376 and uses it
// whenever it is merely NON-NULL (sub_824961D0's prologue: `lwz r11,1376(r24);
// cmplwi 0; bne <use it>`). The rendering thread destroys that object as part of
// executing FDrawSceneCommand, and nothing clears the field. Measured 156 times
// in a single run; the 156th is the crash, because by then the block has been
// reused and its first word is a free-list pointer rather than a vtable.
//
// FIRST ATTEMPT, AND IT WAS WRONG -- recorded because the reason matters.
// Nulling the cached field looked like the correct invariant ("freeing a block
// clears references to it") and I asserted the title would then take a
// create-a-new-one path. IT DOES NOT. The prologue's non-null test is a
// different decision point; the site that actually crashes reads +1376 and
// dereferences it UNCONDITIONALLY, guarded only by the +1396 selector. Nulling
// turned a use-after-free into a null dereference at the same instruction
// (measured: r3 = 0x0, faulting one line earlier). A crash moved is not a crash
// fixed.
//
// SO THE INVARIANT TO OWN IS THE OTHER ONE: do not free a block that is still
// referenced. The title's code requires a live object there and provides no path
// that copes without one, so the port keeps it alive.
//
// The cost is a leaked block per occurrence, and that is a deliberate trade:
// bounded (156 in a full run, each a small pool allocation), against a
// guaranteed crash. The guest's pool simply has slightly less memory to reuse --
// its free-list bookkeeping is untouched because the free never happens.
// THE PRECISE QUESTION, and the last one this line of enquiry needs.
//
// A holder being freed is normal once the title has finished with it, so the free
// alone proves nothing. What matters is the ORDER: if a holder's most recent pool
// event was a FREE, the title is walking a dead object; if it was an ALLOCATION,
// the address was legitimately recycled and the crash is elsewhere again.
namespace
{
struct PoolEvent
{
    uint64_t ordinal = 0;
    bool freed = false;
};
std::mutex g_poolEventMutex;
std::unordered_map<uint32_t, PoolEvent> g_poolEvents;
std::atomic<uint64_t> g_poolOrdinal{0};
} // namespace

void NotePoolEvent(uint32_t address, bool freed)
{
    if (address == 0)
        return;
    std::lock_guard<std::mutex> guard(g_poolEventMutex);
    g_poolEvents[address] = PoolEvent{g_poolOrdinal.fetch_add(1) + 1, freed};
}

bool LastPoolEventWasFree(uint32_t address, uint64_t &ordinal, bool &known)
{
    std::lock_guard<std::mutex> guard(g_poolEventMutex);
    const auto it = g_poolEvents.find(address);
    known = it != g_poolEvents.end();
    if (!known)
        return false;
    ordinal = it->second.ordinal;
    return it->second.freed;
}

bool BlockIsStillReferenced(uint32_t block)
{
    std::lock_guard<std::mutex> guard(g_holderMutex);
    for (const uint32_t holder : g_holders)
    {
        if (ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 1376)) == block)
            return true;
    }
    return false;
}
} // namespace gears::titles::gears1

// ---------------------------------------------------------------------------
// The render fence WAIT (catalog #45).
//
// sub_824453A0 is UE3's FRenderCommandFence wait: it spins on [fence+0] until
// it drops to the threshold in r4, yielding via sub_826128B8 between reads. The
// counter is raised by the game thread in sub_82445278 (lwarx/addi/stwcx. on
// the same word) before a FenceCommand is enqueued, and lowered by that
// command's Execute on the rendering thread.
//
// This is the mechanism that is SUPPOSED to stop the game thread touching an
// object the renderer still owns -- exactly the use-after-free in #45. The loop
// itself reads correctly (guest loads are volatile in this port), so the
// question is not whether it works but whether it RUNS.
//
// The negative is designed: zero waits is reported as the seam never having
// fired, because "the fence is never waited on" and "this probe is broken"
// would otherwise look identical.
extern "C" PPC_FUNC(__imp__sub_824453A0);

namespace
{
std::atomic<uint64_t> g_fenceWaits{0};
std::atomic<uint64_t> g_fenceBlocked{0};
} // namespace

PPC_FUNC(sub_824453A0)
{
    const uint32_t fence = ctx.r3.u32;
    const uint32_t threshold = ctx.r4.u32;
    const uint32_t before = fence ? ByteSwap(*gears::Memory().Translate<uint32_t>(fence)) : 0;

    const uint64_t n = g_fenceWaits.fetch_add(1) + 1;
    const bool blocks = before > threshold;
    if (blocks)
        g_fenceBlocked.fetch_add(1);

    // First few, then only waits that actually block -- a flat cap would hide
    // the interesting ones exactly as it hid the crashing free earlier.
    if (n <= 3 || (blocks && g_fenceBlocked.load() <= 8))
        lucent::info("fence",
                     "wait #{} on {:#x}: counter {} vs threshold {} ->"
                     " {} ({} of {} waits have blocked)",
                     n, fence, before, threshold,
                     blocks ? "WILL BLOCK until the renderer catches up"
                            : "already satisfied, returns immediately",
                     g_fenceBlocked.load(), n);
    __imp__sub_824453A0(ctx, base);
}

namespace
{
std::atomic<uint64_t> g_ringProducerEntries{0};
std::atomic<uint64_t> g_ringProducerOverlaps{0};
} // namespace

namespace gears::titles::gears1
{
void CountRingProducerEntry()
{
    g_ringProducerEntries.fetch_add(1);
    ObserveFrameProductionRingReservation();
}
void CountRingProducerOverlap()
{
    g_ringProducerOverlaps.fetch_add(1);
}
uint64_t RingProducerEntries()
{
    return g_ringProducerEntries.load();
}
uint64_t RingProducerOverlaps()
{
    return g_ringProducerOverlaps.load();
}

void NoteRenderRingReservation(uint32_t start, uint32_t bytes, uint32_t caller)
{
    if (start == 0 || bytes == 0)
        return;
    std::lock_guard<std::mutex> guard(g_ringMutex);
    g_ringReservations.insert_or_assign(start,
                                        RenderRingReservation{.bytes = bytes, .caller = caller});
}

void ReportRenderRingReservationForObject(uint32_t object)
{
    std::lock_guard<std::mutex> guard(g_ringMutex);
    auto next = g_ringReservations.upper_bound(object);
    if (next != g_ringReservations.begin())
    {
        const auto reservation = std::prev(next);
        const uint32_t start = reservation->first;
        const RenderRingReservation &details = reservation->second;
        if (object >= start && uint64_t(object) - start < details.bytes)
        {
            lucent::info("ring",
                         "selected shader callback object {:#x} is +{:#x} in a {}-byte"
                         " render-ring reservation from caller {:#x}",
                         object, object - start, details.bytes, details.caller);
            return;
        }
    }
    lucent::error("ring",
                  "selected shader callback object {:#x} matched no current render-ring"
                  " reservation ({} reservations recorded); allocator provenance is unavailable",
                  object, g_ringReservations.size());
}
} // namespace gears::titles::gears1

// ---------------------------------------------------------------------------
// SuspendRendering / ResumeRendering (catalog #45).
//
// The title has four render commands -- SuspendRendering1/2 and
// ResumeRendering1/2 (identified from their Describe thunks, which return those
// UTF-16 literals). Their Execute bodies do exactly one thing: set or clear a
// global flag at 0x82BFA388.
//
// AND NOTHING READS THAT FLAG. tools/find_addr_refs.py finds exactly two code
// references to it, and both are these two setters. So enqueueing a suspend does
// not actually stop the rendering thread -- same shape as the ring's bIsWriting.
// (Bound: the scanner covers lis+addi and lis+load/store, so a reader that
// obtained the address by another route would be missed.)
//
// This matters for #45 because the game thread's checkpoint restore races the
// rendering thread's container clear. If the title suspends rendering around
// that restore and the suspend is inert, the race is one this port could close
// honestly by honouring the title's own flag. If the title never suspends there,
// this is a dead end and should be recorded as one.
extern "C" PPC_FUNC(__imp__sub_82428FE8);
extern "C" PPC_FUNC(__imp__sub_82428FA8);

PPC_FUNC(sub_82428FE8)
{
    static std::atomic<uint64_t> n{0};
    const uint64_t i = n.fetch_add(1) + 1;
    if (i <= 6)
        lucent::info("suspend",
                     "SuspendRendering executed (#{}) -- sets the flag"
                     " at 0x82BFA388 that nothing reads",
                     i);
    __imp__sub_82428FE8(ctx, base);
}

PPC_FUNC(sub_82428FA8)
{
    static std::atomic<uint64_t> n{0};
    const uint64_t i = n.fetch_add(1) + 1;
    if (i <= 6)
        lucent::info("suspend", "ResumeRendering executed (#{})", i);
    __imp__sub_82428FA8(ctx, base);
}
