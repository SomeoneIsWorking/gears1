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
#include <string>

#include <byteswap.h>
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
        if (reported.fetch_add(1) < 4)
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
// to catch happened on every run. THE SIZE IS r5.
PPC_FUNC(sub_822151F8)
{
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
