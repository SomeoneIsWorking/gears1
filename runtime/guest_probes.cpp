// Probes on specific guest functions, for questions the runtime cannot answer
// from its own side. Each one is a strong `sub_X` that logs and calls through,
// which works for indirect calls too because the function-mapping table holds
// the weak alias.
//
// ---------------------------------------------------------------------------
// The title's own fatal-error path.
//
// The title bug-checks intermittently about 70 seconds in (catalog #44), and
// the panic itself is a RE-ENTRANCY GUARD: sub_828D2FB0 reads a flag, calls
// KeBugCheck(0) if it is already set, and otherwise sets it. So the panic is
// the handler being entered a SECOND time, and the interesting event -- the
// first entry, and whatever caused it -- happens silently before anything the
// runtime logs.
//
// The seam is XenonRecomp's: every guest function is emitted as a weak alias
// `sub_X` over the real body `__imp__sub_X`, so a strong `sub_X` here replaces
// it and can call through. PPCFuncMappings, which serves INDIRECT calls, holds
// the alias too -- so this catches the handler even though nothing in the image
// calls it directly, which is how it is reached.
#include "import_stub.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include <byteswap.h>
#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_backtrace.h"
#include "guest_thread.h"
#include "guest_memory.h"

// TWO ENTRY POINTS, EIGHT BYTES APART, into the same body -- and the caller
// uses the SECOND. Overriding only sub_828D2FB0 caught nothing while the panic
// fired anyway, which is what exposed this: both sites that reach KeBugCheck(0)
// share the return address 0x828D30B0, but one lives in the range the
// recompiler labelled sub_828D2FB0 and the other in sub_828D2FB8. Whatever
// registers this handler registers the +8 entry.
// The chain, from the outside in. Everything below sub_828D0790 is
// unconditional -- sub_828D9FD8 always ends in the fatal exit -- so the DECISION
// is made above sub_828D0790, and that is the only one of these whose caller is
// still unknown. It has no direct callers anywhere in the image, so like the
// handler it is reached through a pointer, and like the handler it can be
// caught here.
extern "C" PPC_FUNC(__imp__sub_828D0790);
extern "C" PPC_FUNC(__imp__sub_828D2FB0);
extern "C" PPC_FUNC(__imp__sub_828D2FB8);

namespace
{
// The re-entrancy flag the handler tests. Reading it on the way IN says whether
// this entry is the one that panics, which is the difference between watching
// the failure and watching its aftermath.
constexpr uint32_t kFatalReentryFlag = 0x82BC9E18;
} // namespace

namespace
{
// Printable runs in a window of guest memory, reported as candidates rather
// than as fact: this is a stack frame, so some of what looks like text will be
// coincidence. Naming it a guess is the difference between a lead and a lie.
void DumpNearbyText(uint32_t around)
{
    if (around == 0)
        return;
    constexpr int32_t kBefore = 512;
    constexpr int32_t kAfter = 2048;
    const uint8_t* memory = gears::Memory().Base();

    std::string run;
    uint32_t runStart = 0;
    uint32_t found = 0;
    for (int32_t offset = -kBefore; offset <= kAfter && found < 8; ++offset)
    {
        const uint32_t address = uint32_t(int64_t(around) + offset);
        const unsigned char c = memory[address];
        if (c >= 0x20 && c <= 0x7E)
        {
            if (run.empty())
                runStart = address;
            run.push_back(char(c));
            continue;
        }
        if (run.size() >= 8)
        {
            lucent::error("fatal", "  text near the fatal frame at {:#x}: \"{}\"",
                          runStart, run);
            ++found;
        }
        run.clear();
    }
}

void ReportEntry(const char* entry, const PPCContext& ctx)
{
    const uint32_t flag = ByteSwap(*gears::Memory().Translate<uint32_t>(kFatalReentryFlag));
    lucent::error("fatal", "the title entered its fatal handler ({}) from {:#x}:"
        " r3={:#x} r4={:#x} r5={:#x} r6={:#x}, re-entry flag {} -- this entry {}",
        entry, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
        flag,
        flag == 1 ? "WILL bug-check" : "is the first, and something already went"
                                       " wrong to get here");

    // The caller (sub_828D9FD8) builds something in its stack frame before
    // handing control here, and the calls it makes on the way look like
    // formatting. If that is an error message it names the failure outright,
    // which is worth more than every register in the report above. Scanning is
    // cheap and happens exactly once per process death.
    DumpNearbyText(ctx.r6.u32);
}
} // namespace

PPC_FUNC(sub_828D2FB0)
{
    ReportEntry("sub_828D2FB0", ctx);
    __imp__sub_828D2FB0(ctx, base);
}

PPC_FUNC(sub_828D2FB8)
{
    ReportEntry("sub_828D2FB8", ctx);
    __imp__sub_828D2FB8(ctx, base);
}

// sub_828D3118 is `r5 = 0; r4 = 1; goto handler` and sub_828D3128 is
// `r5 = 1; ...` -- and the handler panics precisely when that argument is zero.
// So the KeBugCheck is HARDCODED into this path by the title: it is the "must
// not return" tail of a deliberate terminate, not a field the runtime failed to
// populate. Two wrappers, one that ends the process and one that does not.
// THIS IS _purecall. The address 0x828D0790 appears 1837 times in the loaded
// image, always inside runs of code pointers -- vtables -- and often in
// consecutive slots. That is what a compiler emits for a PURE VIRTUAL slot: one
// shared stub that terminates the process if anyone ever calls it.
//
// So reaching here is not the title deciding to quit. It is the title calling a
// pure virtual function, which means the object's vtable is an abstract base's:
// the classic causes are a virtual call during construction or destruction, or
// a call on an object another thread has already destroyed. On a per-frame
// object walk, rare and timing-dependent, a lifetime race is the shape that
// fits.
PPC_FUNC(sub_828D0790)
{
    lucent::error("fatal", "PURE VIRTUAL CALL (_purecall at sub_828D0790), from"
        " {:#x}: r3={:#x} r4={:#x} r5={:#x} r6={:#x}. The object's vtable is an"
        " abstract base's -- it is under construction, under destruction, or"
        " already destroyed", uint32_t(ctx.lr),
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);

    // WHO was walking the object. The pure-virtual stub is reached through a
    // vtable slot, so the link register alone names one anonymous dispatch; the
    // frames above it are the subsystem doing the walking, which is the thing
    // that has to be identified to explain the lifetime violation.
    lucent::error("fatal", "  guest stack: {}",
        gears::FormatGuestBacktrace(ctx.r1.u32, uint32_t(ctx.lr)));

    // Which vtable the object actually carries is the whole question now: it
    // will be some abstract base's, and WHICH one names the class whose
    // lifetime is being violated.
    if (ctx.r3.u32 != 0)
    {
        const uint32_t vtable = ByteSwap(*gears::Memory().Translate<uint32_t>(ctx.r3.u32));
        const bool plausible = vtable >= PPC_IMAGE_BASE &&
                               vtable < PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
        uint32_t slot1 = 0;
        if (plausible)
            slot1 = ByteSwap(*gears::Memory().Translate<uint32_t>(vtable + 4));
        lucent::error("fatal", "  the object it was called on ({:#x}) has vtable"
            " {:#x} ({}), slot 1 = {:#x}", ctx.r3.u32, vtable,
            plausible ? "a real vtable -- find which class it belongs to"
                      : "NOT in the image, so the object is corrupt rather than"
                        " merely half-constructed",
            slot1);

        // Is the object inside the render ring? If it is, the read cursor is
        // sitting on a command's PAYLOAD rather than its header, which is a
        // framing failure rather than a lifetime one -- and those have
        // completely different fixes.
        const uint32_t ringData = ByteSwap(*gears::Memory().Translate<uint32_t>(0x82C0CB24));
        const uint32_t ringEnd = ByteSwap(*gears::Memory().Translate<uint32_t>(0x82C0CB24 + 4));
        if (ringData != 0 && ringEnd > ringData)
            lucent::error("fatal", "  the render ring spans {:#x}..{:#x}; the"
                " object at {:#x} is {}", ringData, ringEnd, ctx.r3.u32,
                ctx.r3.u32 >= ringData && ctx.r3.u32 < ringEnd
                    ? "INSIDE it -- the read cursor is off a command boundary"
                    : "outside it, so this is not a ring framing failure");
    }
    __imp__sub_828D0790(ctx, base);
}

// ---------------------------------------------------------------------------
// The engine's pool allocator, which asks for 2.6 GB on the save path
// (catalog #45).
//
// The chain below it is known -- sub_82214F50 -> sub_82214B58 ->
// NtAllocateVirtualMemory -- and one of the two call sites rounds its size up
// to a 64 KB boundary and passes it straight through, so the number is decided
// ABOVE here. Walking further by reading costs a hop per attempt; logging the
// arguments and the caller answers it in one run.
// WHERE THE SIZE IS COMPUTED, at last: sub_82170408 is a container growth
// helper that does `size = this->elementCount(at +8) * r4`, and its caller
// sub_8232B548 fills that count in from `abs(load32(object + 80))`. So the whole
// 2.6 GB is `2 * abs(field80)`, and every function below this one is a
// pass-through. Reporting both factors says WHICH of them is wrong, which is
// the only remaining question.
// The string serialiser above it. sub_8232B548 takes (archive, string) and
// writes UE3's FString form: an int32 length, NEGATIVE when the text needs
// UTF-16, then abs(length) characters. The 2.6 GB is that length times two, so
// the string it was handed has a corrupt length field -- and WHICH string names
// the subsystem that produced it. Guessing from what appears nearby in the log
// is how the last three explanations here died.
// THE FEEDER for the archive blob that the deferred handler deserialises.
// sub_821BA838 builds a SAVING archive over the same TArray at object+420 that
// sub_821B4620 later reads -- but it returns immediately when [object+424] is
// zero. If that gate is shut, nothing ever fills the array, which is exactly
// the state the deserialise probe reports on every run.
extern "C" PPC_FUNC(__imp__sub_821BA838);

PPC_FUNC(sub_821BA838)
{
    static std::atomic<uint64_t> seen{0};
    const uint32_t object = ctx.r3.u32;
    const uint32_t gate = ByteSwap(*gears::Memory().Translate<uint32_t>(object + 424));
    const uint32_t before = ByteSwap(*gears::Memory().Translate<uint32_t>(object + 424 - 4));

    __imp__sub_821BA838(ctx, base);

    if (seen.fetch_add(1) < 6)
    {
        const uint32_t data = ByteSwap(*gears::Memory().Translate<uint32_t>(object + 420));
        const uint32_t count = ByteSwap(*gears::Memory().Translate<uint32_t>(object + 424));
        lucent::info("probe", "save-blob feeder on object {:#x}: gate [+424]={:#x}"
            " -> {}; afterwards the array at +420 is {} ({} bytes at {:#x}),"
            " returned {:#x}", object, gate,
            gate == 0 ? "RETURNED EARLY, nothing serialised" : "proceeded",
            data == 0 || count == 0 ? "EMPTY" : "populated", count, data,
            ctx.r3.u32);
        (void)before;
    }
}

extern "C" PPC_FUNC(__imp__sub_8232B548);

namespace
{
thread_local uint32_t t_serialisingArchive = 0;
thread_local uint32_t t_archiveIsSaving = 0;
} // namespace

PPC_FUNC(sub_8232B548)
{
    static std::atomic<uint64_t> seen{0};
    // The length lives in this function's own frame, at r1-128+80. On a SAVING
    // archive it is computed from the string; on a LOADING one the store is
    // skipped and Archive::Serialize reads it out of the file. So the value has
    // to be read AFTER the call -- reading the argument at entry, as the first
    // version of this probe did, sees nothing on the loading path and reports
    // silence as if there were no problem.
    (void)seen;
    const uint32_t archive = ctx.r3.u32;
    // Recorded at ENTRY and read back by the growth probe below, because the
    // oversized allocation -- and the segfault that follows it -- happen INSIDE
    // this call. Anything logged after it may never be reached.
    t_serialisingArchive = archive;
    t_archiveIsSaving = ByteSwap(*gears::Memory().Translate<uint32_t>(archive + 20));

    // THE QUESTION THIS ANSWERS: is the archive's byte array populated at all?
    // The oversized allocation is gone, but "the garbage got smaller" and "the
    // data is now there" look identical from the outside, and only one of them
    // is progress.
    {
        static std::atomic<uint64_t> reported{0};
        const uint32_t holder = ByteSwap(*gears::Memory().Translate<uint32_t>(archive + 112));
        const uint32_t buffer = holder ? ByteSwap(*gears::Memory().Translate<uint32_t>(holder)) : 0;
        const uint32_t count = holder ? ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 4)) : 0;
        // THE CAP USED TO BE A FLAT `< 4`, AND IT HID THE ONLY CASE THAT
        // MATTERS. The first four deserialises happen early, before anything
        // fills the carrier, and are harmless; the one that crashes happens
        // much later, after the carrier IS populated. Reporting only the first
        // four therefore showed "the array is always empty" -- a conclusion the
        // instrument could not have contradicted, because it had stopped
        // looking. A populated array is now ALWAYS reported: it is the new
        // information, and the state at the crash is the whole question.
        const bool populated = buffer != 0 && count != 0;
        if (reported.fetch_add(1) < 4 || populated)
        {
            // When the array is empty the read goes to LOW GUEST MEMORY, so the
            // length the title gets is whatever physical RAM happens to hold at
            // that offset. Printing it says whether an "improvement" is the data
            // arriving or merely the garbage changing.
            const uint32_t low4 = ByteSwap(*gears::Memory().Translate<uint32_t>(4));
            // WHERE THE BLOB IS SUPPOSED TO COME FROM. sub_821B4620 copies the
            // global TArray at 0x82BFB36C into object+420 before deserialising
            // (guest 0x821B4938: dest = r17+420, src = 0x82BFB36C), and that
            // global is filled by the Kismet "LoadChapter" op sub_82207E98. So
            // if the global is empty, the copy faithfully produces an empty
            // array and the read that follows has nothing to read.
            constexpr uint32_t kCarrier = 0x82BFB36C;
            const uint32_t carrierData = ByteSwap(*gears::Memory().Translate<uint32_t>(kCarrier));
            const uint32_t carrierCount = ByteSwap(*gears::Memory().Translate<uint32_t>(kCarrier + 4));
            lucent::info("probe", "  the global carrier at {:#x} is {} ({} bytes"
                " at {:#x}) -- this is what gets copied into the object before"
                " the read", kCarrier,
                carrierData == 0 || carrierCount == 0 ? "EMPTY" : "populated",
                carrierCount, carrierData);
            lucent::info("probe", "FString deserialise #{}: archive array is {}"
                " ({} bytes at {:#x}); guest address 4 currently holds {:#x}",
                reported.load(),
                buffer == 0 || count == 0 ? "EMPTY -- nothing filled it"
                                          : "populated",
                count, buffer, low4);
        }
    }

    __imp__sub_8232B548(ctx, base);

    t_serialisingArchive = 0;
}

extern "C" PPC_FUNC(__imp__sub_82170408);

PPC_FUNC(sub_82170408)
{
    static std::atomic<uint64_t> seen{0};
    const uint32_t count = ByteSwap(*gears::Memory().Translate<uint32_t>(ctx.r3.u32 + 8));
    const uint64_t size = uint64_t(count) * ctx.r4.u32;
    if (size >= (64u << 20) && seen.fetch_add(1) < 8)
    {
        lucent::error("probe", "container growth to {} elements x {} bytes ="
            " {:#x}: the count came from a field holding {:#x} ({} as a signed"
            " integer, {} as a float)", count, ctx.r4.u32, size, count,
            int32_t(count), *reinterpret_cast<const float*>(&count));
        // WHICH THREAD, because the only reason this is filed under saves is
        // that it appears next to the content mount in the log -- and adjacency
        // is not ownership.
        lucent::error("probe", "  on guest thread '{}', stack: {}",
            gears::GuestThreadName() ? gears::GuestThreadName() : "?",
            gears::FormatGuestBacktrace(ctx.r1.u32, uint32_t(ctx.lr)));
        if (t_serialisingArchive != 0)
        {
            // The archive is a MEMORY reader -- no file read happens between
            // the mount and the failure -- so its buffer pointer and position
            // are in this object, and they say what it is actually chewing on.
            // 128 bytes, not 64: the archive is built inline on its caller's
            // stack and keeps a pointer to its data source at +112, which the
            // first version of this dump stopped just short of.
            lucent::Line dump;
            dump.add("  archive {:#x}:", t_serialisingArchive);
            for (uint32_t i = 0; i < 128; i += 4)
                dump.add(" {:08x}", ByteSwap(*gears::Memory().Translate<uint32_t>(
                    t_serialisingArchive + i)));
            dump.flush(lucent::Level::Error, "probe");

            // Slot 1 of the vtable is Archive::Serialize -- the function that
            // actually produced the bad length. Naming it turns "the bytes are
            // wrong" into a specific reader to go and read.
            const uint32_t vtable = ByteSwap(
                *gears::Memory().Translate<uint32_t>(t_serialisingArchive));
            lucent::Line slots;
            slots.add("  archive vtable {:#x} slots:", vtable);
            for (uint32_t i = 0; i < 8; ++i)
                slots.add(" {:08x}", ByteSwap(
                    *gears::Memory().Translate<uint32_t>(vtable + i * 4)));
            slots.flush(lucent::Level::Error, "probe");

            // Serialize (vtable slot 1, sub_821B5C28) is a MEMORY reader with
            // no bounds check whatsoever:
            //   buffer = *(uint32*)(this + 112) -> +0
            //   memcpy(dest, buffer + this->offset(+108), length)
            // So the bad length came out of this buffer, and dumping it is the
            // "which bytes" question answered directly.
            const uint32_t sourceObject = ByteSwap(
                *gears::Memory().Translate<uint32_t>(t_serialisingArchive + 112));
            const uint32_t position = ByteSwap(
                *gears::Memory().Translate<uint32_t>(t_serialisingArchive + 108));
            if (sourceObject != 0)
            {
                const uint32_t buffer = ByteSwap(
                    *gears::Memory().Translate<uint32_t>(sourceObject));
                const uint32_t count = ByteSwap(
                    *gears::Memory().Translate<uint32_t>(sourceObject + 4));
                lucent::error("probe", "  reading at offset {} from a {}-byte"
                    " array at {:#x} (holder {:#x})", position, count, buffer,
                    sourceObject);
                if (buffer == 0)
                {
                    // The buffer is NULL and Serialize does not bounds-check, so
                    // the read was memcpy(dest, 0 + offset, n) -- it read LOW
                    // GUEST MEMORY. If those bytes were zero the length would be
                    // zero and nothing would go wrong, so what is actually down
                    // there decides whether this is fatal.
                    lucent::Line low;
                    low.add("  buffer is NULL, so it read guest address {}."
                            " Low memory holds:", position);
                    for (uint32_t i = 0; i < 32; i += 4)
                        low.add(" {:08x}",
                            ByteSwap(*gears::Memory().Translate<uint32_t>(i)));
                    low.flush(lucent::Level::Error, "probe");
                }
                if (buffer != 0)
                {
                    lucent::Line bytes;
                    bytes.add("  first bytes:");
                    for (uint32_t i = 0; i < 48; ++i)
                        bytes.add(" {:02x}",
                            *gears::Memory().Translate<uint8_t>(buffer + i));
                    bytes.flush(lucent::Level::Error, "probe");
                }
            }
        }
        if (t_serialisingArchive != 0)
            lucent::error("probe", "  inside FString serialisation on a {}"
                " archive ({:#x}). {}",
                t_archiveIsSaving ? "SAVING" : "LOADING", t_serialisingArchive,
                t_archiveIsSaving
                    ? "Saving: the length was computed from the string, so the"
                      " string itself is corrupt."
                    : "LOADING: the length was READ OUT OF THE ARCHIVE, so the"
                      " bytes fed to it are wrong -- a file-reading bug on our"
                      " side, not a title one.");
    }
    __imp__sub_82170408(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82214F50);
extern "C" PPC_FUNC(__imp__sub_822151F8);

PPC_FUNC(sub_82214F50)
{
    static std::atomic<uint64_t> seen{0};
    // Only the outsized requests: this is a pool allocator on a normal path and
    // a line per call would bury the one that matters.
    // The allocator's own vtable, once. Slot +4 is allocate (this function) and
    // slot +12 is FREE -- the realloc path calls it as `lwz r11,0(this); lwz
    // r11,12(r11); bctrl`. Naming that address is what makes it possible to
    // probe the free, which is the open question for the use-after-free in
    // catalog #45: the constructor/destructor seam is blind because the compiler
    // inlines them.
    static std::atomic<bool> namedVtable{false};
    if (!namedVtable.exchange(true) && ctx.r3.u32 != 0)
    {
        const uint32_t vt = ByteSwap(*gears::Memory().Translate<uint32_t>(ctx.r3.u32));
        lucent::Line line;
        line.add("pool allocator {:#x} vtable {:#x} slots:", ctx.r3.u32, vt);
        for (uint32_t i = 0; i < 6; ++i)
            line.add(" +{}={:08x}", i * 4,
                ByteSwap(*gears::Memory().Translate<uint32_t>(vt + i * 4)));
        line.flush(lucent::Level::Info, "probe");
    }

    if (ctx.r4.u32 >= (64u << 20) && seen.fetch_add(1) < 8)
        lucent::error("probe", "pool allocation of {:#x} bytes ({} MB) requested"
            " from {:#x}: r3={:#x} r5={:#x} r6={:#x} r7={:#x}", ctx.r4.u32,
            ctx.r4.u32 >> 20, uint32_t(ctx.lr), ctx.r3.u32, ctx.r5.u32,
            ctx.r6.u32, ctx.r7.u32);
    __imp__sub_82214F50(ctx, base);
}

// One level up, and it is a REALLOC, not an alloc: the prologue keeps r4 as the
// existing pointer and r5 as the new size, and a null pointer falls through to
// the plain-allocate path that calls sub_82214F50 with the size in r4. Filtering
// on r4 here therefore tests a POINTER against a size threshold, which is why
// the first version of this probe never fired while the allocation it was meant
namespace
{
// The realloc's own frame, captured before calling through so the walk starts
// from the right place rather than from wherever the probe happens to be.
thread_local uint32_t g_reallocStack = 0;
thread_local uint32_t g_reallocLr = 0;
} // namespace

namespace gears { void ReportReallocOfCached(uint32_t block, uint32_t newSize, uint32_t from); }


// to catch happened on every run. THE SIZE IS r5.
PPC_FUNC(sub_822151F8)
{
    // IS THIS REALLOC THE ONE THAT ORPHANS A CACHED OBJECT? r4 is the existing
    // block and r5 the new size; the free of the old block happens inside, at
    // 0x822153E4. If r4 is currently cached at some holder's +1376, then THIS
    // call is what leaves that field dangling -- and r5 names the size that
    // drove the growth, which is the thing to check against what the console
    // would have asked for.
    if (ctx.r4.u32 != 0)
    {
        g_reallocStack = ctx.r1.u32;
        g_reallocLr = uint32_t(ctx.lr);
        gears::ReportReallocOfCached(ctx.r4.u32, ctx.r5.u32, uint32_t(ctx.lr));
    }

    static std::atomic<uint64_t> seen{0};
    if (ctx.r5.u32 >= (64u << 20) && seen.fetch_add(1) < 8)
    {
        lucent::error("probe", "engine realloc of {:#x} bytes ({} MB) requested"
            " from {:#x}: allocator={:#x} existing pointer={:#x} r6={:#x}"
            " r7={:#x}", ctx.r5.u32, ctx.r5.u32 >> 20, uint32_t(ctx.lr),
            ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, ctx.r7.u32);
        // Every level at once. The size arrives here as an argument and each
        // frame above is another chance to see where it was computed; probing
        // them one at a time costs a run each.
        lucent::error("probe", "  guest stack: {}",
            gears::FormatGuestBacktrace(ctx.r1.u32, uint32_t(ctx.lr)));
    }
    __imp__sub_822151F8(ctx, base);
}

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

// One slot per guest thread name seen in each path. A second distinct name is
// the whole finding, so it is reported the moment it appears.
std::mutex g_ringMutex;
std::string g_drainThreadName;
std::string g_producerThreadName;

void NoteRingThread(const char* which, std::string& slot, std::atomic<uint32_t>& count)
{
    const char* name = gears::GuestThreadName();
    const std::string current = name ? name : "?";
    std::lock_guard<std::mutex> guard(g_ringMutex);
    if (slot.empty())
    {
        slot = current;
        lucent::info("ring", "{} entered by guest thread '{}'", which, current);
        return;
    }
    if (slot != current)
    {
        // THE SINGLE-PRODUCER/SINGLE-CONSUMER ASSUMPTION IS BROKEN. FRingBuffer
        // has no protection against this, and this is exactly the corruption it
        // produces.
        lucent::error("ring", "{} entered by a SECOND guest thread: '{}' after"
            " '{}'. FRingBuffer assumes one producer and one consumer; this is"
            " the corruption", which, current, slot);
        count.fetch_add(1);
        slot = current;
    }
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

    NoteRingThread("the render-ring allocator", g_producerThreadName, g_producerThreads);

    // WHICH CALL SITE, per thread. Two producers on a ring with a non-atomic
    // commit is a lost-update race; whether it is OUR bug depends on whether the
    // rendering thread is SUPPOSED to enqueue here, and the caller identifies
    // that. Recorded as a set so a hot path cannot flood the log.
    {
        const char* rawName = gears::GuestThreadName();
        const std::string name = rawName ? rawName : "?";
        const uint32_t from = uint32_t(ctx.lr);
        static std::mutex sitesMutex;
        static std::map<std::string, std::set<uint32_t>> sites;
        std::lock_guard<std::mutex> guard(sitesMutex);
        auto& forThread = sites[name];
        if (forThread.insert(from).second && forThread.size() <= 12)
        {
            lucent::info("ring", "producer '{}' enqueues from {:#x} (distinct"
                " call site {} for this thread)", name, from, forThread.size());

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
                    lucent::error("ring", "  the stack walk yielded only {}"
                        " frame(s) -- too few to name the route. Treat this as"
                        " the WALKER failing, not as the route being short",
                        frames.size());
                else
                    lucent::error("ring", "  route from '{}': {}", name,
                        gears::FormatGuestBacktrace(ctx.r1.u32, uint32_t(ctx.lr)));
            }
        }
    }

    __imp__sub_8221CBA8(ctx, base);
}

PPC_FUNC(sub_82444EF0)
{
    NoteRingThread("the render-ring drain loop", g_drainThreadName, g_drainThreads);

    const uint32_t data = ByteSwap(*gears::Memory().Translate<uint32_t>(kRenderRing + 0));
    const uint32_t dataEnd = ByteSwap(*gears::Memory().Translate<uint32_t>(kRenderRing + 4));
    lucent::info("ring", "drain loop starting: Data={:#x} DataEnd={:#x}"
        " (a bad object pointer inside this range means the read cursor is on a"
        " command's payload rather than its header)", data, dataEnd);

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

void ReportCarrier(const char* who)
{
    const uint32_t data = ByteSwap(*gears::Memory().Translate<uint32_t>(kChapterCarrier));
    const uint32_t count = ByteSwap(*gears::Memory().Translate<uint32_t>(kChapterCarrier + 4));
    lucent::info("chapter", "{}: the carrier at {:#x} is {} ({} bytes at {:#x})",
        who, kChapterCarrier, data == 0 || count == 0 ? "EMPTY" : "populated",
        count, data);
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
size_t g_destructionCursor = 0;
uint64_t g_constructed = 0;
uint64_t g_destroyed = 0;
} // namespace

namespace gears { void ReportArchiveLifetime(uint32_t object); void CheckFreedWhileCached(uint32_t freed); void ReportReallocOfCached(uint32_t, uint32_t, uint32_t); }

namespace gears
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
    lucent::error("lifetime", "{} constructions and {} destructions seen through"
        " the out-of-line bodies. If both are zero the compiler has inlined them"
        " and this seam cannot see the class at all -- that is a blind probe,"
        " not evidence of absence", g_constructed, g_destroyed);
    for (const Destruction& d : g_destructions)
    {
        if (d.object == object)
        {
            lucent::error("lifetime", "  THE CRASHING OBJECT {:#x} WAS DESTROYED"
                " (destruction #{}) from {:#x} -- use after free, and that is the"
                " caller that freed it", object, d.ordinal, d.from);
            return;
        }
    }
    lucent::error("lifetime", "  object {:#x} is not among the last {}"
        " destructions of this class -- either it was freed longer ago than the"
        " ring remembers, or it was freed by some other path", object,
        kDestructionSlots);
}
} // namespace gears

PPC_FUNC(sub_8242C098)
{
    {
        std::lock_guard<std::mutex> guard(g_lifetimeMutex);
        ++g_constructed;
    }
    __imp__sub_8242C098(ctx, base);
}

PPC_FUNC(sub_8242C180)
{
    const uint32_t object = ctx.r3.u32;
    const uint32_t from = uint32_t(ctx.lr);
    {
        std::lock_guard<std::mutex> guard(g_lifetimeMutex);
        Destruction& slot = g_destructions[g_destructionCursor % kDestructionSlots];
        slot.object = object;
        slot.from = from;
        slot.ordinal = g_destroyed;
        ++g_destructionCursor;
        ++g_destroyed;
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

namespace gears
{
void ReportLastFree(uint32_t address)
{
    std::lock_guard<std::mutex> guard(g_freeMutex);
    const auto it = g_lastFreeSite.find(address);
    if (it == g_lastFreeSite.end())
    {
        lucent::error("lifetime", "  {:#x} was never freed through the pool"
            " ({} frees seen). If that seems wrong, check this seam fires at"
            " all before concluding anything from it", address,
            g_frees.load());
        return;
    }
    lucent::error("lifetime", "  {:#x} WAS FREED, most recently from {:#x}."
        " That is the caller to look at", address, it->second);
}
} // namespace gears

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
    static const uint32_t watched = [] {
        const uint32_t value = lucent::config::number("WATCH_FREE", 0);
        if (value != 0)
            lucent::info("lifetime", "watching for the release of {:#x}", value);
        return value;
    }();
    gears::CheckFreedWhileCached(address);

    if (watched != 0 && address == watched)
        lucent::error("lifetime", "WATCHED ADDRESS {:#x} FREED from {:#x}"
            " (free #{})", address, from, g_frees.load());

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
extern "C" PPC_FUNC(__imp__sub_824961D0);

namespace
{
std::mutex g_holderMutex;
std::set<uint32_t> g_holders;
std::atomic<uint64_t> g_holderVisits{0};
} // namespace

namespace gears
{
// Called from the pool-free probe: does any live holder still cache this block?
void CheckFreedWhileCached(uint32_t freed)
{
    // COUNTED IN FULL, REPORTED BY NOVELTY. A flat "first N" cap reported the
    // first six of these and nothing else -- and the six that matter are the
    // LAST ones, immediately before the crash. So: every occurrence is counted,
    // the first three are shown to establish the shape, and after that only a
    // change of holder is shown. The running total goes out with each line so a
    // reader can see how much is being elided.
    static std::atomic<uint64_t> total{0};
    static std::atomic<uint64_t> shown{0};
    static uint32_t lastHolder = 0;

    std::lock_guard<std::mutex> guard(g_holderMutex);
    for (const uint32_t holder : g_holders)
    {
        const uint32_t cached =
            ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 1376));
        if (cached != freed)
            continue;

        const uint64_t n = total.fetch_add(1) + 1;
        const bool novel = holder != lastHolder;
        lastHolder = holder;
        if (shown.load() < 3 || novel)
        {
            shown.fetch_add(1);
            lucent::error("lifetime", "FREEING {:#x} WHILE IT IS STILL CACHED at"
                " holder {:#x}+1376 (occurrence {}). Nothing nulls that field,"
                " and the next call uses it on the strength of it being"
                " non-null", freed, holder, n);
        }
    }
}

// Reports only when the block being reallocated is one a holder still caches.
void ReportReallocOfCached(uint32_t block, uint32_t newSize, uint32_t from)
{
    static std::atomic<uint64_t> total{0};
    static std::atomic<uint64_t> shown{0};
    std::lock_guard<std::mutex> guard(g_holderMutex);
    for (const uint32_t holder : g_holders)
    {
        if (ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 1376)) != block)
            continue;
        const uint64_t n = total.fetch_add(1) + 1;
        // First few plus every hundredth: enough to see the shape and the tail
        // without the flat cap that hid the crashing occurrence last time.
        if (shown.load() < 4 || n % 100 == 0)
        {
            shown.fetch_add(1);
            lucent::error("lifetime", "REALLOC of {:#x} to {} bytes from {:#x}"
                " -- that block is cached at holder {:#x}+1376, so this call"
                " orphans it (occurrence {})", block, newSize, from, holder, n);
            // WHO IS DELETING IT. The link register only names the generic
            // allocator wrapper, which every delete goes through; the caller
            // that actually decided to destroy this object is further out, and
            // only a stack walk reaches it. Reported as a walker failure if the
            // walk is too short to be an answer.
            const std::vector<uint32_t> frames =
                gears::GuestBacktrace(g_reallocStack, g_reallocLr);
            if (frames.size() < 4)
                lucent::error("lifetime", "  stack walk gave {} frame(s) --"
                    " too few to name the deleter; treat as the WALKER failing",
                    frames.size());
            else
                lucent::error("lifetime", "  deleter: {}",
                    gears::FormatGuestBacktrace(g_reallocStack, g_reallocLr));
        }
    }
}

void ReportHolderSeam()
{
    lucent::info("lifetime", "checkpoint holder seam: {} visit(s), {} distinct"
        " holder(s){}", g_holderVisits.load(), g_holders.size(),
        g_holders.empty() ? " -- the seam NEVER FIRED, so the free check above"
                            " proves nothing" : "");
}
} // namespace gears

PPC_FUNC(sub_824961D0)
{
    g_holderVisits.fetch_add(1);
    {
        std::lock_guard<std::mutex> guard(g_holderMutex);
        g_holders.insert(ctx.r3.u32);
    }
    // WHICH THREAD USES THE CACHE. The deleter runs on the rendering thread
    // (its stack ends at the drain loop's Execute call, 0x82444f7c). If the
    // USER is a different thread, this is a cross-thread use-after-free and the
    // two open issues are the same bug seen from two ends.
    {
        static std::mutex m;
        static std::set<std::string> seen;
        const char* raw = gears::GuestThreadName();
        const std::string name = raw ? raw : "?";
        std::lock_guard<std::mutex> guard(m);
        if (seen.insert(name).second)
            lucent::info("lifetime", "the cached-object user sub_824961D0 runs"
                " on guest thread '{}'", name);
    }
    __imp__sub_824961D0(ctx, base);
}

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
    const uint32_t before =
        fence ? ByteSwap(*gears::Memory().Translate<uint32_t>(fence)) : 0;

    const uint64_t n = g_fenceWaits.fetch_add(1) + 1;
    const bool blocks = before > threshold;
    if (blocks)
        g_fenceBlocked.fetch_add(1);

    // First few, then only waits that actually block -- a flat cap would hide
    // the interesting ones exactly as it hid the crashing free earlier.
    if (n <= 3 || (blocks && g_fenceBlocked.load() <= 8))
        lucent::info("fence", "wait #{} on {:#x}: counter {} vs threshold {} ->"
            " {} ({} of {} waits have blocked)", n, fence, before, threshold,
            blocks ? "WILL BLOCK until the renderer catches up"
                   : "already satisfied, returns immediately",
            g_fenceBlocked.load(), n);
    __imp__sub_824453A0(ctx, base);
}
