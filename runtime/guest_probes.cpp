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
#include <sys/syscall.h>
#include <unistd.h>
#include <set>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

#include <byteswap.h>
#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_backtrace.h"
#include "guest_thread.h"
#include "guest_memory.h"

// Pool allocation/free bookkeeping, defined further down but used by the
// allocator probe above its definition.
namespace gears
{
void NotePoolEvent(uint32_t address, bool freed);
bool LastPoolEventWasFree(uint32_t address, uint64_t& ordinal, bool& known);
// ULinkerLoad bookkeeping, defined at the bottom of this file.
void NoteLinkerEntry(uint32_t holder);
void ReportMapChangeSeams();
} // namespace gears

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

namespace
{
// Counted for every call, reported at the abort, so "the map-name probe printed
// nothing" is always distinguishable from "the return-address filter is wrong".
std::atomic<uint64_t> g_fstringLoads{0};
std::atomic<uint64_t> g_fstringFromMapChange{0};
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

    // AND THE THREE CALLS THAT BUILD THE MAP NAME, specifically. The load path
    // has an early-out that would produce exactly the empty string observed:
    //
    //   bctrl                  ; Serialize(&len, 4)
    //   lwz  r11,16(r28)       ; a flag on the ARCHIVE
    //   cmpwi cr6,r11,0
    //   beq  cr6,0x8232b72c    ; ZERO -> skip the whole deserialise
    //
    // "the length read was zero or garbage" and "nothing was read at all" have
    // different causes -- the bytes versus the archive's state -- and telling
    // them apart by guessing the FArchive member layout is the kind of
    // assumption that has cost this issue days. So both are read here.
    g_fstringLoads.fetch_add(1);
    const uint32_t lr = uint32_t(ctx.lr);
    const bool forMapName = lr >= 0x821B4620 && lr < 0x821B5000;
    const uint32_t stringOut = ctx.r4.u32;
    const uint32_t flagAt16 =
        ByteSwap(*gears::Memory().Translate<uint32_t>(archive + 16));

    __imp__sub_8232B548(ctx, base);

    if (forMapName)
    {
        // THE ARCHIVE'S OWN FIELDS, on the first call only. The first read
        // swallowed 10089 characters out of a 385-byte save, so the cursor or the
        // byte array is wrong before a single string is parsed -- and which of
        // those it is cannot be settled by guessing an FArchive subclass layout.
        // The carrier is printed alongside so the two can be compared: it is
        // known good (385 bytes, the save file, byte-identical to chapter37.sav).
        static std::atomic<bool> dumped{false};
        if (!dumped.exchange(true))
        {
            constexpr uint32_t kCarrier = 0x82BFB36C;
            lucent::debug("linker", "  the carrier holds {} bytes at {:#x}"
                " -- this is the blob the archive should be reading",
                ByteSwap(*gears::Memory().Translate<uint32_t>(kCarrier + 4)),
                ByteSwap(*gears::Memory().Translate<uint32_t>(kCarrier)));
            // +112 IS A REFERENCE, NOT THE ARRAY. FMemoryReader holds
            // `const TArray<BYTE>& Bytes`, so the descriptor is one indirection
            // further out. Reading +112/+116 as though they were data/count is
            // what made an earlier probe report "2147483648 bytes at 0x900006b0"
            // -- an impossible descriptor that was really two unrelated fields.
            const uint32_t arrayRef =
                ByteSwap(*gears::Memory().Translate<uint32_t>(archive + 112));
            if (uint64_t(arrayRef) + 8 < PPC_MEMORY_SIZE)
            {
                const uint32_t data =
                    ByteSwap(*gears::Memory().Translate<uint32_t>(arrayRef));
                const uint32_t count =
                    ByteSwap(*gears::Memory().Translate<uint32_t>(arrayRef + 4));
                lucent::debug("linker", "  the archive's byte array is at {:#x}:"
                    " data {:#x}, count {}{}", arrayRef, data, int32_t(count),
                    (data == 0 || count == 0)
                        ? "  <- EMPTY, so every read comes back as nothing and the"
                          " carrier never reached this archive"
                        : "");
            }
            else
            {
                lucent::debug("linker", "  the archive's byte-array reference"
                    " {:#x} is not readable, so the archive was constructed over"
                    " something that is not an array at all", arrayRef);
            }

            lucent::Line row;
            row.add("  archive {:#x} fields:", archive);
            for (uint32_t offset = 0; offset < 128; offset += 4)
                row.add(" +{}={:#x}", offset,
                    ByteSwap(*gears::Memory().Translate<uint32_t>(archive + offset)));
            row.flush(lucent::Level::Error, "linker");
        }

        const auto word = [&](uint32_t address) {
            return ByteSwap(*gears::Memory().Translate<uint32_t>(address));
        };
        const int32_t length = int32_t(word(stringOut + 4));
        const uint64_t n = g_fstringFromMapChange.fetch_add(1) + 1;
        lucent::debug("linker", "map-name FString deserialise #{}: archive {:#x}"
            " (+16={:#x}, +20={:#x}) -> FString at {:#x} data {:#x} length {}{}",
            n, archive, flagAt16, t_archiveIsSaving, stringOut,
            word(stringOut), length,
            flagAt16 == 0
                ? "  <- THE FLAG AT +16 IS ZERO, so the load path skipped the"
                  " deserialise entirely and left the string untouched: the cause"
                  " is the ARCHIVE'S STATE, not the bytes in it"
                : (length == 0
                    ? "  <- the deserialise RAN and read a length of zero, so the"
                      " cause is the BYTES, not the archive's state"
                    : ""));
    }

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
    gears::NotePoolEvent(ctx.r3.u32, false);
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

namespace gears
{
// Entries and overlaps, so a zero overlap count carries its denominator.
void CountRingProducerEntry();
void CountRingProducerOverlap();
uint64_t RingProducerEntries();
uint64_t RingProducerOverlaps();
} // namespace gears



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
    // THE TID IS PART OF THE IDENTITY, not decoration. 'host' is the DEFAULT
    // name for every thread that has never run guest code (guest_thread.cpp),
    // so two different host threads entering the producer compared EQUAL and
    // the detector stayed silent about them -- which is exactly catalog #44's
    // open question, WHICH host thread is entering the guest's render ring.
    // Appending the tid makes the comparison distinguish them as well as
    // naming them.
    const std::string current = std::string(name ? name : "?") + " (tid " +
        std::to_string(long(syscall(SYS_gettid))) + ")";
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
        // THE OS THREAD ID, because 'host' is not an identity. It is the
        // DEFAULT name for any thread that never entered guest code
        // (guest_thread.cpp), so every host thread reports the same string and
        // catalog #44's own next question -- WHICH host thread enters the
        // producer, and whether it is a guest callback running on one of ours
        // -- cannot be answered from the name alone. The tid can be joined
        // against the thread that created it.
        lucent::error("ring", "{} entered by a SECOND thread: '{}' after '{}'"
            " (producer entries so far: {}, overlaps caught by the wait: {})."
            " FRingBuffer assumes one producer and one consumer. Each name"
            " carries its tid because 'host' is the DEFAULT for every thread"
            " that has never run guest code, so two host threads would"
            " otherwise be indistinguishable", which, current, slot,
            gears::RingProducerEntries(), gears::RingProducerOverlaps());
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
    // accesses, and two producers DO interleave -- measured: the game thread and
    // the rendering thread both enter here, ~30 alternations per run. The commit
    // is a non-atomic read-modify-write on WritePointer, so a lost update leaves
    // it mid-command and the consumer then reads a header that is not one.
    //
    // So this waits for the flag the title already maintains. It is not a lock
    // invented here; it completes a protocol the title defines and hardware made
    // unnecessary. Bounded, because a producer that reserves and never commits
    // would otherwise hang the process -- and if that bound is ever hit, it is
    // reported rather than silently ignored.
    if (ctx.r4.u32 == kRenderRing)
    {
        const uint32_t writingFlag = kRenderRing + 16;
        constexpr int kMaxSpins = 20000;
        int spins = 0;
        while (ByteSwap(*gears::Memory().Translate<uint32_t>(writingFlag)) != 0)
        {
            if (++spins > kMaxSpins)
            {
                lucent::error("ring", "bIsWriting stayed set for {} spins --"
                    " proceeding anyway. Either a producer reserved without"
                    " committing, or this wait is wrong; it must not hang the"
                    " process either way", kMaxSpins);
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
            gears::CountRingProducerOverlap();
        static std::atomic<uint64_t> engagements{0};
        if (spins != 0)
            lucent::info("ring", "producer wait ENGAGED: {} spin(s) for another"
                " producer's commit (engagement {})", spins,
                engagements.fetch_add(1) + 1);
    }

    NoteRingThread("the render-ring allocator", g_producerThreadName, g_producerThreads);
    gears::CountRingProducerEntry();

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

namespace gears { void ReportArchiveLifetime(uint32_t object); bool BlockIsStillReferenced(uint32_t block); void NotePoolEvent(uint32_t, bool); bool LastPoolEventWasFree(uint32_t, uint64_t&, bool&); }

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
    const auto it = g_lastDestruction.find(object);
    if (it != g_lastDestruction.end())
    {
        lucent::error("lifetime", "  THE CRASHING OBJECT {:#x} WAS DESTROYED"
            " (destruction #{} of {}) with lr {:#x} -- use after free, and that"
            " return address names the caller that deleted it", object,
            it->second.ordinal, g_destroyed, it->second.from);
        return;
    }
    lucent::error("lifetime", "  object {:#x} appears in NONE of the {}"
        " destructions this seam recorded ({} distinct addresses), so this class"
        " never destroyed it -- either it is not an FArchiveAsync at all, or it"
        " died through a path that does not run this destructor", object,
        g_destroyed, g_lastDestruction.size());
}
} // namespace gears

// FArchiveAsync::FArchiveAsync(const TCHAR*). See NoteArchiveAsync at the bottom
// of this file for why ArIsError is the whole question.
namespace gears { void NoteArchiveAsync(uint32_t self, uint32_t name); }

PPC_FUNC(sub_8242C098)
{
    const uint32_t self = ctx.r3.u32;
    const uint32_t name = ctx.r4.u32;
    {
        std::lock_guard<std::mutex> guard(g_lifetimeMutex);
        ++g_constructed;
    }
    __imp__sub_8242C098(ctx, base);
    gears::NoteArchiveAsync(self, name);
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
    gears::NotePoolEvent(address, true);

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

namespace
{
std::mutex g_holderMutex;
std::set<uint32_t> g_holders;
std::atomic<uint64_t> g_holderVisits{0};
} // namespace

namespace gears
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
struct PoolEvent { uint64_t ordinal = 0; bool freed = false; };
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

bool LastPoolEventWasFree(uint32_t address, uint64_t& ordinal, bool& known)
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
} // namespace gears

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


namespace
{
std::atomic<uint64_t> g_ringProducerEntries{0};
std::atomic<uint64_t> g_ringProducerOverlaps{0};
} // namespace

namespace gears
{
void CountRingProducerEntry() { g_ringProducerEntries.fetch_add(1); }
void CountRingProducerOverlap() { g_ringProducerOverlaps.fetch_add(1); }
uint64_t RingProducerEntries() { return g_ringProducerEntries.load(); }
uint64_t RingProducerOverlaps() { return g_ringProducerOverlaps.load(); }
} // namespace gears

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
        lucent::info("suspend", "SuspendRendering executed (#{}) -- sets the flag"
            " at 0x82BFA388 that nothing reads", i);
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

// ---------------------------------------------------------------------------
// ULinkerLoad::CreateLoader, its memo, and the ONE branch that leaves the memo
// dangling (catalog #45).
//
// THE CLASSES ARE NOW NAMED, from the code rather than from the shape of the
// crash. sub_824961D0 is ULinkerLoad::CreateLoader; its r3/r24 is the
// ULinkerLoad. The fields the crash turns on:
//
//   +0x0cc/+0x0d0  ULinker::Filename        (FString: data pointer, ArrayNum)
//   +0x14c         ULinker::LoadFlags       (bit 0 = LOAD_SeekFree, bit 16 used)
//   +0x0e4/+0x0ec  FArchive::ArVer / ArLicenseeVer (set to 374 at 0x82496a20)
//   +0x560 (1376)  ULinkerLoad::Loader      (FArchive*) -- THE MEMO
//   +0x574 (1396)  bHasSerializedPackageFileSummary -- THE GUARD
//   +0x584/+0x588  bTimeLimitExceeded / IsTimeLimitExceededCallCount
//
// sub_8242C098 is FArchiveAsync::FArchiveAsync(const TCHAR*). Its tail is the
// whole reason this probe exists:
//
//   8242c144  lwz   r3,-0x7e44(r11)   ; GFileManager
//   8242c14c  lwz   r11,0xc(r10)      ; vtable slot 3 = FileSize
//   8242c154  bctrl
//   8242c15c  stw   r3,0x78(r31)      ; FileSize = GFileManager->FileSize(name)
//   8242c164  blt   cr6,0x8242c174
//   8242c168  stw   r30,0x2c(r31)     ; r30 = 0 : ArIsError = FALSE
//   8242c174  stw   r28,0x2c(r31)     ; r28 = 1 : ArIsError = TRUE
//
// So ArIsError is set for exactly one reason: FileSize came back NEGATIVE, i.e.
// the file could not be found. And CreateLoader's seek-free branch is
//
//   82496520  bl    0x8242c098        ; Loader = new FArchiveAsync(Filename)
//   82496538  stw   r3,0x560(r24)     ; MEMOISED BEFORE THE ERROR IS CHECKED
//   8249653c  lwz   r11,0x2c(r3)      ; ArIsError
//   82496544  beq   cr6,0x824967e4    ; clean -> carry on
//   82496550  lwz   r11,0(r3) ; li r4,1 ; bctrl   ; DELETE the loader
//   82496564  ... Localize("Errors","OpenFailed") ... report ...
//   82496620  b     0x824967e4        ; and FALL THROUGH to the shared tail
//   ...
//   82496a4c  lwz   r11,0x574(r24)    ; bHasSerializedPackageFileSummary
//   82496a54  bne   cr6,0x82496a9c
//   82496a58  lwz   r3,0x560(r24)     ; THE DELETED LOADER
//   82496a60  lwz   r11,0x34(r11)     ; slot 13 = TotalSize
//   82496a68  bctrl                   ; <- the crash, lr 0x82496a6c
//
// If that branch runs, the crash needs no second thread and no race: the object
// is created, memoised, deleted and called through inside a single call, and the
// block is still on the pool's free list when it is used -- which is exactly
// what the fault dump shows (word0 = the block 0xC0 further on, word1 = 1).
//
// THE NEGATIVE IS THE POINT OF THIS PROBE. If no FArchiveAsync is ever
// constructed with ArIsError set, that branch never runs and this whole reading
// is dead -- and the report says so, with the number of constructions as its
// denominator, plus the blind spot that only LOAD_SeekFree packages reach this
// constructor at all.
namespace
{
// A guest UTF-16 (big-endian) string, as much of it as is worth printing.
std::string GuestWideString(uint32_t address, uint32_t maxChars = 160)
{
    std::string out;
    if (address == 0 || uint64_t(address) + 2 >= PPC_MEMORY_SIZE)
        return "<null>";
    for (uint32_t i = 0; i < maxChars; ++i)
    {
        const uint32_t at = address + i * 2;
        if (uint64_t(at) + 2 >= PPC_MEMORY_SIZE)
            break;
        const uint16_t unit = __builtin_bswap16(
            *gears::Memory().Translate<uint16_t>(at));
        if (unit == 0)
            break;
        out.push_back(unit < 0x80 ? char(unit) : '?');
    }
    return out;
}

std::mutex g_archiveMutex;
std::atomic<uint64_t> g_archiveAsyncBuilt{0};
std::atomic<uint64_t> g_archiveAsyncFailed{0};
std::set<std::string> g_archiveAsyncFailedNames;

// Per-ULinkerLoad entry history for sub_824961D0. The one datum that separates
// the two candidate mechanisms is the value of the memo AT ENTRY on the call
// that crashes: non-zero means the create block was skipped and the object died
// between calls (an external deleter); zero means this very call built it, and
// the only thing that can have destroyed it is CreateLoader's own error branch.
struct LinkerVisit
{
    uint64_t visits = 0;
    uint32_t loaderAtEntry = 0;
    std::string filename;
};
std::mutex g_linkerMutex;
std::unordered_map<uint32_t, LinkerVisit> g_linkerVisits;
} // namespace

namespace gears
{
void NoteLinkerEntry(uint32_t holder)
{
    if (holder == 0 || uint64_t(holder) + 1400 >= PPC_MEMORY_SIZE)
        return;
    const uint32_t loader =
        ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 0x560));
    const uint32_t nameData =
        ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 0xcc));
    const uint32_t nameLen =
        ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 0xd0));
    std::lock_guard<std::mutex> guard(g_linkerMutex);
    LinkerVisit& visit = g_linkerVisits[holder];
    visit.visits += 1;
    visit.loaderAtEntry = loader;
    if (nameLen != 0)
        visit.filename = GuestWideString(nameData);
}

// Called from the bad-indirect-call reporter, which is where every run of this
// repro ends, so it is the only exit that is guaranteed to be reached.
void ReportLinkerState(uint32_t holder)
{
    const uint64_t built = g_archiveAsyncBuilt.load();
    const uint64_t failed = g_archiveAsyncFailed.load();
    if (built == 0)
        lucent::debug("linker", "FArchiveAsync seam NEVER FIRED (0 constructions"
            " through sub_8242C098), so nothing below about ArIsError is"
            " evidence either way -- check the seam before reading on");
    else if (failed == 0)
        lucent::debug("linker", "FArchiveAsync: {} constructed, ZERO with"
            " ArIsError. CreateLoader's open-failed branch therefore never ran,"
            " and the create-delete-use-in-one-call reading is DEAD. Blind spot:"
            " only LOAD_SeekFree packages reach this constructor; the plain"
            " CreateFileReader path at 0x82496638 does not", built);
    else
    {
        lucent::debug("linker", "FArchiveAsync: {} constructed, {} of them with"
            " ArIsError set -- i.e. GFileManager->FileSize() returned negative"
            " and the file was NOT FOUND", built, failed);
        std::lock_guard<std::mutex> guard(g_archiveMutex);
        for (const std::string& name : g_archiveAsyncFailedNames)
            lucent::debug("linker", "  could not open: '{}'", name);
    }

    if (holder == 0 || uint64_t(holder) + 1400 >= PPC_MEMORY_SIZE)
    {
        lucent::debug("linker", "r24 {:#x} is not a readable guest pointer, so"
            " the ULinkerLoad cannot be dumped", holder);
        return;
    }
    const auto word = [&](uint32_t offset) {
        return ByteSwap(*gears::Memory().Translate<uint32_t>(holder + offset));
    };
    const uint32_t nameData = word(0xcc);
    const uint32_t nameLen = word(0xd0);
    lucent::debug("linker", "ULinkerLoad {:#x}: Filename '{}' ({} chars),"
        " LoadFlags {:#x}, Loader(+1376) {:#x},"
        " bHasSerializedPackageFileSummary(+1396) {:#x}, ArVer {}",
        holder, nameLen ? GuestWideString(nameData) : std::string("<empty>"),
        nameLen, word(0x14c), word(0x560), word(0x574), word(0xe4));

    {
        std::lock_guard<std::mutex> guard(g_linkerMutex);
        const auto it = g_linkerVisits.find(holder);
        if (it == g_linkerVisits.end())
            lucent::debug("linker", "  this ULinkerLoad never entered"
                " sub_824961D0 through the probe ({} distinct linkers were"
                " recorded), so the entry state below is unknown -- treat that"
                " as the PROBE failing, not as a finding",
                g_linkerVisits.size());
        else
            lucent::debug("linker", "  it entered CreateLoader {} time(s); on the"
                " most recent entry Loader was {:#x}, which is the {} path. {}",
                it->second.visits, it->second.loaderAtEntry,
                it->second.loaderAtEntry ? "MEMO-HIT (creation skipped)"
                                         : "CREATE",
                it->second.loaderAtEntry
                    ? "So the object was destroyed BETWEEN calls and the deleter"
                      " is outside this function."
                    : "So this same call created the loader, and the only code"
                      " that can have destroyed it before 0x82496a58 is"
                      " CreateLoader's own open-failed branch at 0x82496550.");
    }

    const uint32_t loader = word(0x560);
    if (loader != 0)
    {
        uint64_t ordinal = 0;
        bool known = false;
        const bool freed = LastPoolEventWasFree(loader, ordinal, known);
        lucent::debug("linker", "  the memoised Loader {:#x}: {}", loader,
            !known ? "was never seen by the pool probe at all -- so either it did"
                     " not come from this allocator or the probe missed it"
            : freed ? "its most recent pool event was a FREE"
                    : "its most recent pool event was an ALLOCATION (which is"
                      " also what a block RE-ISSUED to a different owner looks"
                      " like, so this does not mean it is still the archive)");
        ReportLastFree(loader);
        // AND WHO DELETED IT. The destructor probe on sub_8242C180 records the
        // return address of every FArchiveAsync destruction; if the crashing
        // loader is in there, the lr names the exact site. 0x82496564 would be
        // CreateLoader's own open-failed branch.
        ReportArchiveLifetime(loader);
    }
    ReportMapChangeSeams();
}
} // namespace gears

void gears::NoteArchiveAsync(uint32_t self, uint32_t nameAddress)
{
    const std::string name = GuestWideString(nameAddress);
    const uint64_t n = g_archiveAsyncBuilt.fetch_add(1) + 1;
    const uint32_t isError =
        ByteSwap(*gears::Memory().Translate<uint32_t>(self + 0x2c));
    const int32_t fileSize = int32_t(
        ByteSwap(*gears::Memory().Translate<uint32_t>(self + 0x78)));
    if (isError == 0)
    {
        // A handful of successes, to prove the seam reads the right fields
        // before any conclusion is drawn from a zero failure count.
        if (n <= 4)
            lucent::info("linker", "FArchiveAsync #{} at {:#x} opened '{}'"
                " ({} bytes)", n, self, name, fileSize);
        return;
    }

    const uint64_t bad = g_archiveAsyncFailed.fetch_add(1) + 1;
    bool novel = false;
    {
        std::lock_guard<std::mutex> guard(g_archiveMutex);
        novel = g_archiveAsyncFailedNames.insert(name).second;
    }
    // Every NEW name, plus the first few, plus every hundredth: the interesting
    // case is the one that is never elided.
    if (novel || bad <= 4 || bad % 100 == 0)
        lucent::debug("linker", "FArchiveAsync #{} at {:#x} FAILED TO OPEN '{}'"
            " (FileSize {} -> ArIsError). CreateLoader has already memoised it"
            " at ULinkerLoad+1376 and is about to delete it and carry on"
            " (failure {} of {} constructions)", n, self, name, fileSize, bad,
            n);
}

// ---------------------------------------------------------------------------
// WHERE THE EMPTY PACKAGE NAME COMES FROM (catalog #45).
//
// Measured: the ULinkerLoad that crashes has Filename "" (0 chars), LoadFlags
// 0x81, and its FArchiveAsync failed because GFileManager->FileSize("") is -1.
// So the crash is downstream of a package load REQUESTED WITH AN EMPTY NAME,
// and the two functions above it in the guest stack are:
//
//   sub_82426D98  UGameEngine::PrepareMapChange(TArray<FName>& LevelNames)
//                 -- r3 = engine, r4 = the FName array. At 0x82426fa0 it does
//                    FName::ToString into a stack FString and at 0x82426fd4
//                    passes it to LoadPackageAsync WITHOUT a FindPackageFile
//                    check (the only FindPackageFile, at 0x82426f18, guards the
//                    "<name>_LOC" companion load, not the map itself).
//   sub_8242AC78  UObject::LoadPackageAsync(const FString& Name, cb, userdata)
//                 -- appends an FAsyncPackage carrying that string to the global
//                    array at 0x82BFC5D4. Nothing in it validates the name.
//
// So the name that ends up in ULinkerLoad::Filename is whatever the caller
// computed, and the caller is sub_821B4620 -- the deferred checkpoint handler
// that deserialises the map name out of the checkpoint blob (0x821b4f14 calls
// PrepareMapChange, 0x821b4f30 then pumps ProcessAsyncLoading in a spin).
//
// THE NEGATIVE IS DESIGNED. If neither seam fires, the report says so with its
// own visit count, because "the map load never goes through here" and "the probe
// is attached to the wrong function" produce identical silence otherwise.
extern "C" PPC_FUNC(__imp__sub_82426D98);
extern "C" PPC_FUNC(__imp__sub_8242AC78);

namespace
{
std::atomic<uint64_t> g_prepareMapChanges{0};
std::atomic<uint64_t> g_asyncLoadRequests{0};
std::atomic<uint64_t> g_emptyAsyncLoadRequests{0};
} // namespace

namespace gears
{
void ReportMapChangeSeams()
{
    lucent::debug("linker", "PrepareMapChange seam: {} call(s); LoadPackageAsync"
        " seam: {} request(s), {} of them with an EMPTY name.{}",
        g_prepareMapChanges.load(), g_asyncLoadRequests.load(),
        g_emptyAsyncLoadRequests.load(),
        g_asyncLoadRequests.load() == 0
            ? " ZERO requests means this seam NEVER FIRED -- the async load is"
              " started somewhere else and nothing below is evidence."
            : "");
}
} // namespace gears

PPC_FUNC(sub_82426D98)
{
    const uint64_t n = g_prepareMapChanges.fetch_add(1) + 1;
    const uint32_t array = ctx.r4.u32;
    if (array != 0 && uint64_t(array) + 12 < PPC_MEMORY_SIZE)
    {
        const uint32_t data = ByteSwap(*gears::Memory().Translate<uint32_t>(array));
        const uint32_t count =
            ByteSwap(*gears::Memory().Translate<uint32_t>(array + 4));
        lucent::info("linker", "PrepareMapChange #{}: {} level name(s) at {:#x}",
            n, count, data);
        // The FNames themselves, raw. Resolving an FName to text needs the
        // title's name table; the raw pair is enough to tell "index 0 / None"
        // from a real entry, which is the distinction that matters here.
        for (uint32_t i = 0; i < count && i < 8; ++i)
        {
            const uint32_t at = data + i * 8;
            if (uint64_t(at) + 8 >= PPC_MEMORY_SIZE)
                break;
            lucent::info("linker", "  level[{}] FName index {} number {}", i,
                ByteSwap(*gears::Memory().Translate<uint32_t>(at)),
                ByteSwap(*gears::Memory().Translate<uint32_t>(at + 4)));
        }
    }
    else
        lucent::debug("linker", "PrepareMapChange #{}: r4 {:#x} is not a readable"
            " array pointer, so the level list could not be read", n, array);
    __imp__sub_82426D98(ctx, base);
}

PPC_FUNC(sub_8242AC78)
{
    const uint64_t n = g_asyncLoadRequests.fetch_add(1) + 1;
    const uint32_t str = ctx.r3.u32;
    uint32_t data = 0;
    int32_t length = -1;
    if (str != 0 && uint64_t(str) + 8 < PPC_MEMORY_SIZE)
    {
        data = ByteSwap(*gears::Memory().Translate<uint32_t>(str));
        length = int32_t(ByteSwap(*gears::Memory().Translate<uint32_t>(str + 4)));
    }
    const bool empty = length <= 0 || data == 0;
    if (empty)
        g_emptyAsyncLoadRequests.fetch_add(1);
    // Every empty request, plus the first few of any kind. An empty one is the
    // event this seam exists for, so it is never elided.
    if (empty || n <= 8)
        lucent::debug("linker", "LoadPackageAsync #{}: name FString at {:#x} ="
            " data {:#x} len {} -> '{}'{}", n, str, data, length,
            (data && length > 0) ? GuestWideString(data) : std::string(),
            empty ? "  <-- EMPTY. This request is what produces a ULinkerLoad"
                    " with no filename, whose FArchiveAsync then fails to open"
                    " and is used after being deleted at 0x82496550"
                  : "");
    __imp__sub_8242AC78(ctx, base);
}

// WHERE THE MAP NAME IS LOST.
//
// #45's crash is a retail bug on the package-open-failed path, and we reach it
// because UGameEngine::PrepareMapChange receives a single level name whose FName
// index is 0 -- NAME_None. The array it receives is built in sub_821B4620:
//
//   lwz  r11,140(r1)        ; pick which string to use
//   cmpwi cr6,r11,0
//   bne  cr6,0x821b4b64
//   mr   r4,r28             ; ... or r30 on the other branch
//   li   r7,1 ; li r6,1 ; li r5,0
//   addi r3,r1,128          ; out FName
//   bl   0x82364678         ; FName::FName(out, string, ...)
//   ...
//   ld   r11,128(r1)
//   std  r11,0(r22)         ; the FName becomes element 0 of the array
//
// So either the string handed to the constructor is empty, or the constructor is
// being asked to FIND rather than ADD and the name is not in the table yet. This
// probe reads the string and the resulting index, which separates those two.
//
// FILTERED BY RETURN ADDRESS, because FName construction is one of the hottest
// paths in the engine and an unfiltered log is useless. The filter is also the
// thing most likely to make this print nothing, so the count of calls seen from
// ANYWHERE is reported too: "0 from the map-change site, N from elsewhere" is a
// measurement, while a bare silence would be indistinguishable from a probe that
// never ran.
extern "C" PPC_FUNC(__imp__sub_82364678);

namespace
{
std::atomic<uint64_t> g_fnameCalls{0};
std::atomic<uint64_t> g_fnameFromMapChange{0};
} // namespace

PPC_FUNC(sub_82364678)
{
    g_fnameCalls.fetch_add(1);

    const uint32_t lr = uint32_t(ctx.lr);
    const bool fromMapChange = lr >= 0x821B4620 && lr < 0x821B5000;
    const uint32_t out = ctx.r3.u32;
    const uint32_t stringAddress = ctx.r4.u32;

    if (!fromMapChange)
    {
        __imp__sub_82364678(ctx, base);
        return;
    }

    // Read the string the title is naming. TCHAR is UTF-16 on this console, so
    // the characters are two bytes big-endian; a narrow string would show as
    // interleaved NULs and is worth seeing rather than guessing about.
    std::string text;
    bool readable = uint64_t(stringAddress) + 2 < PPC_MEMORY_SIZE;
    if (stringAddress != 0 && readable)
    {
        for (uint32_t i = 0; i < 64; ++i)
        {
            const uint32_t at = stringAddress + i * 2;
            if (uint64_t(at) + 2 >= PPC_MEMORY_SIZE)
                break;
            const uint16_t unit = ByteSwap(*gears::Memory().Translate<uint16_t>(at));
            if (unit == 0)
                break;
            text.push_back(unit < 0x80 ? char(unit) : '?');
        }
    }

    __imp__sub_82364678(ctx, base);

    const uint32_t index =
        (out != 0 && uint64_t(out) + 4 < PPC_MEMORY_SIZE)
            ? ByteSwap(*gears::Memory().Translate<uint32_t>(out))
            : 0xFFFFFFFFu;

    const uint64_t n = g_fnameFromMapChange.fetch_add(1) + 1;
    // THE ONE LINE A NORMAL RUN SHOULD PRINT. The checkpoint restore is the
    // milestone this whole investigation was about, and a run that silently stops
    // restoring it would otherwise look identical to a run that works. An index
    // of 0 is NAME_None, which is exactly the failure that produced #45.
    if (n == 1)
        lucent::info("linker", "checkpoint restore: the map name resolved to '{}'"
            " (FName index {:#x}){}", text, index,
            index == 0 ? "  <- NAME_None, so the restore has REGRESSED: the title"
                         " will ask for a package called 'None' (catalog #45)"
                       : "");

    lucent::debug("linker", "FName for the map change #{}: string at {:#x} = '{}'"
        " ({} chars{}), find-mode r5={} r6={} r7={} -> INDEX {:#x}{}",
        n, stringAddress, text, text.size(),
        stringAddress == 0 ? ", NULL POINTER"
        : !readable       ? ", UNREADABLE ADDRESS"
        : text.empty()    ? ", EMPTY STRING -- this is why the name is None"
                          : "",
        ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, index,
        index == 0 ? "  <- NAME_None, so the package request will be 'None'" : "");
}

namespace gears
{
// Reported at the abort, so the zero case is never silent.
void ReportMapNameProbe()
{
    lucent::debug("linker", "map-name probe: {} FName construction(s) seen from"
        " the map-change site out of {} in the whole run. A zero on the left with"
        " a non-zero on the right means the return-address filter"
        " (0x821B4620..0x821B5000) is wrong, NOT that the name is fine",
        g_fnameFromMapChange.load(), g_fnameCalls.load());
}
} // namespace gears

// WHY THE MAP NAME DESERIALISES EMPTY.
//
// sub_8232B548 is FArchive& operator<<(FArchive&, FString&), and its load path
// has an early-out that would produce exactly the empty string observed:
//
//   lwz  r11,0(r28) ; lwz r11,4(r11) ; bctrl   ; Serialize(&len, 4)
//   lwz  r11,16(r28)                           ; a flag on the ARCHIVE
//   cmpwi cr6,r11,0
//   beq  cr6,0x8232b72c                        ; ZERO -> skip the whole thing
//
// So either the length read from the archive is zero or garbage, or the archive
// flag at +16 is zero and nothing is read at all. Those have completely
// different causes -- a data problem versus an archive that is not in a state to
// load -- and guessing the FArchive member layout to tell them apart is exactly
// the kind of assumption that has cost this issue days. Read it instead.
//
// FILTERED to the three calls in sub_821B4620 that build the map name, because
// this function ran 78,278 times in the run that first noticed it. The filter is
// the most likely reason for this to print nothing, so the unfiltered total is
// reported alongside: "0 of N" is a measurement about the filter, while silence
// would be indistinguishable from a probe that never ran.
namespace gears
{
void ReportFStringProbe()
{
    lucent::debug("linker", "map-name FString probe: {} deserialise(s) seen from"
        " the map-change site out of {} in the whole run. Zero on the left with a"
        " non-zero right means the return-address filter is wrong, NOT that the"
        " deserialise is fine", g_fstringFromMapChange.load(),
        g_fstringLoads.load());
}
} // namespace gears

// THE FUNCTION THAT DECIDES WHETHER THE CHECKPOINT CARRIER IS COPIED.
//
// sub_821B4620 copies the global carrier at 0x82BFB36C into object+420 -- the
// array the checkpoint archive is built over -- ONLY when this returns zero. It
// is returning non-zero, so the copy is skipped and the archive is empty, which
// is the head of the whole #45 chain.
//
// Read from the guest code (53 instructions, all of it):
//
//   lwz  r11,4(r4)         ; the out-parameter's count
//   beq  -> literal path   ; count == 0
//   addi r11,r11,-1 ; bne -> other path   ; count >= 2
//   ; literal path (count 0 or 1):
//   addi r4,r11,-4228      ; a string literal at 0x8209EF7C
//   bl   0x821704f8        ; FString from that literal
//   addi r4,r31,420        ; object+420
//   bl   0x821b5f30        ; try to load it
//   ; tail, both paths:
//   addi r3,r31,420
//   bl   0x8232f010        ; -> the RETURN VALUE, derived from object+420
//
// So it attempts to populate object+420 itself and reports whether that worked.
// Non-zero therefore claims the array is populated -- while the archive built
// over it measures as data 0, count 0. One of those two readings is wrong, and
// this probe prints both sides of the same array in the same breath so the
// contradiction cannot survive.
extern "C" PPC_FUNC(__imp__sub_821B94D8);

PPC_FUNC(sub_821B94D8)
{
    static std::atomic<uint64_t> seen{0};
    const uint64_t n = seen.fetch_add(1) + 1;

    const uint32_t object = ctx.r3.u32;
    const uint32_t param = ctx.r4.u32;
    const uint32_t arrayAddress = object + 420;

    const auto word = [&](uint32_t address) -> uint32_t {
        if (uint64_t(address) + 4 >= PPC_MEMORY_SIZE)
            return 0xDEADDEADu;
        return ByteSwap(*gears::Memory().Translate<uint32_t>(address));
    };

    const uint32_t paramCount = word(param + 4);
    const uint32_t beforeData = word(arrayAddress);
    const uint32_t beforeCount = word(arrayAddress + 4);

    __imp__sub_821B94D8(ctx, base);

    const uint32_t afterData = word(arrayAddress);
    const uint32_t afterCount = word(arrayAddress + 4);
    const uint32_t result = ctx.r3.u32;

    if (n > 4)
        return;

    // The literal it tries to load, read out of the image. This is the name the
    // title uses when the caller supplied none, and it says what the title
    // thinks it is loading.
    std::string literal;
    for (uint32_t i = 0; i < 48; ++i)
    {
        const uint16_t unit = ByteSwap(*gears::Memory().Translate<uint16_t>(
            0x8209EF7Cu + i * 2));
        if (unit == 0)
            break;
        literal.push_back(unit < 0x80 ? char(unit) : '?');
    }

    constexpr uint32_t kCarrier = 0x82BFB36C;
    // THE OUT-PARAMETER'S CONTENTS, not just its count. Gate #1 succeeds with a
    // count of 14 and gate #2 fails with 0, and the two paths differ ONLY in
    // that -- so what the caller supplied is the question, and "count 14" alone
    // does not answer it. Read as UTF-16, which is what TCHAR is here; a narrow
    // string would show as interleaved NULs and is worth seeing rather than
    // assuming.
    std::string supplied;
    const uint32_t suppliedData = word(param);
    if (suppliedData != 0 && uint64_t(suppliedData) + 2 < PPC_MEMORY_SIZE)
    {
        for (uint32_t i = 0; i < 64; ++i)
        {
            const uint32_t at = suppliedData + i * 2;
            if (uint64_t(at) + 2 >= PPC_MEMORY_SIZE)
                break;
            const uint16_t unit =
                ByteSwap(*gears::Memory().Translate<uint16_t>(at));
            if (unit == 0)
                break;
            supplied.push_back(unit < 0x80 ? char(unit) : '?');
        }
    }

    lucent::debug("linker", "carrier gate #{}: object {:#x}, out-param {:#x}"
        " data {:#x} count {} = '{}' ({} path), fallback literal '{}'",
        n, object, param, suppliedData, paramCount, supplied,
        paramCount <= 1 ? "literal" : "other", literal);
    lucent::debug("linker", "  object+420 before: data {:#x} count {} -> after:"
        " data {:#x} count {}; RETURNED {:#x}", beforeData, int32_t(beforeCount),
        afterData, int32_t(afterCount), result);
    lucent::debug("linker", "  the carrier meanwhile holds {} bytes at {:#x}",
        int32_t(word(kCarrier + 4)), word(kCarrier));
    lucent::debug("linker", "  => {}", result != 0
        ? (afterCount != 0
            ? "non-zero AND the array is populated, so the skip is correct and the"
              " emptiness measured later means something CLEARS it afterwards"
            : "NON-ZERO WHILE THE ARRAY IS EMPTY -- it claims success it did not"
              " achieve, and that is why the carrier is never copied")
        : "zero, so the caller should copy the carrier; if the array is still"
          " empty later, the COPY is what failed");
}

// WHERE THE SAVE-DEVICE CHECKPOINT LOAD GIVES UP.
//
// sub_821B6800 is the save-device loader that sub_821B5F30 calls, and whose
// result becomes sub_821B94D8's return value on the literal path. Reading it
// (guest 0x821B6800) it is a syscall sequence: XamContentCreateEx with the
// overlapped form, XamGetOverlappedResult, then sub_821B5DD8 to BUILD THE PATH,
// then CreateFile / GetFileSize / ReadFile. On failure of the open it returns
// GetLastError; on a GetFileSize of -1 it returns a stale register.
//
// The measured behaviour is that it returns 0 -- success -- having populated
// nothing, and that NO FILE IS EVER OPENED between the content mount and the
// gate returning (verified with the fs debug channel ON, against hundreds of
// logged opens elsewhere including "not found" ones). So it gives up, or
// succeeds vacuously, BEFORE reaching CreateFile.
//
// This reports the return value and the path the builder produced. An empty path
// would mean the builder is the problem; a good path with no open attempt would
// mean it returned before getting there.
extern "C" PPC_FUNC(__imp__sub_821B6800);
extern "C" PPC_FUNC(__imp__sub_821B5DD8);

namespace
{
std::atomic<uint64_t> g_saveLoaderCalls{0};
uint32_t g_lastBuiltPathAddress = 0;
} // namespace

// The path builder, shared by the save WRITE and this read -- so whatever it
// produces here is the same name the checkpoint was written under.
PPC_FUNC(sub_821B5DD8)
{
    __imp__sub_821B5DD8(ctx, base);
    g_lastBuiltPathAddress = ctx.r3.u32;
}

PPC_FUNC(sub_821B6800)
{
    const uint64_t n = g_saveLoaderCalls.fetch_add(1) + 1;
    g_lastBuiltPathAddress = 0;

    __imp__sub_821B6800(ctx, base);

    if (n > 4)
        return;

    const uint32_t result = ctx.r3.u32;

    // The built path, as narrow bytes: this builder writes into a global that
    // starts life as "save:\" and has a name appended, so it is 8-bit here
    // rather than UTF-16.
    std::string path;
    if (g_lastBuiltPathAddress != 0)
    {
        for (uint32_t i = 0; i < 64; ++i)
        {
            const uint32_t at = g_lastBuiltPathAddress + i;
            if (uint64_t(at) + 1 >= PPC_MEMORY_SIZE)
                break;
            const char c = char(*gears::Memory().Translate<uint8_t>(at));
            if (c == 0)
                break;
            path.push_back(c);
        }
    }

    lucent::debug("linker", "save-device checkpoint load #{}: RETURNED {:#x} ({});"
        " the path builder {}",
        n, result,
        result == 0 ? "SUCCESS, so the caller believes a checkpoint was loaded"
                    : "failure, which is what makes the caller copy the carrier",
        g_lastBuiltPathAddress == 0
            ? "was NOT REACHED, so it gave up before building a path at all"
            : std::string("produced '") + path + "'");
}

// THE SAVE-DEVICE LOADER'S SYSCALL SEQUENCE, from the thunks.
//
// sub_821B6800 returns 0 -- success -- while populating nothing, and the
// disassembly says the only route to 0 runs through li r30,0 at 0x821b6930,
// downstream of ReadFile succeeding, which needs CreateFile to have succeeded,
// which would have logged an [fs] line. It returned 0 and nothing was logged, so
// a premise is wrong.
//
// HOOKED AT THE THUNKS DELIBERATELY. The obvious probe -- an override on the path
// builder sub_821B5DD8 -- is STRUCTURALLY BLIND: it lives in ppc_recomp.6.cpp
// alongside its caller, and clang folds intra-TU calls through the weak alias so
// the override is never entered. It duly reported "not reached" and I believed it
// for a while. These three thunks are in ppc_recomp.97.cpp, a different
// translation unit, so their overrides do fire.
//
// Every one reports its own denominator: calls seen from inside the loader
// against calls seen anywhere. A zero on the left with a non-zero on the right is
// a fact about the return-address filter; two zeros mean the override is not
// being entered at all, which is the failure this comment exists to avoid
// repeating.
extern "C" PPC_FUNC(__imp__sub_82611E90);   // CreateFile
extern "C" PPC_FUNC(__imp__sub_82612390);   // ReadFile
extern "C" PPC_FUNC(__imp__sub_826124F8);   // GetLastError

namespace
{
constexpr uint32_t kLoaderStart = 0x821B6800;
constexpr uint32_t kLoaderEnd = 0x821B6980;

struct ThunkCounts { std::atomic<uint64_t> inLoader{0}, anywhere{0}; };
ThunkCounts g_createFile, g_readFile, g_lastError;

bool FromLoader(const PPCContext& ctx)
{
    const uint32_t lr = uint32_t(ctx.lr);
    return lr >= kLoaderStart && lr < kLoaderEnd;
}

std::string NarrowGuestString(uint32_t address)
{
    std::string text;
    if (address == 0)
        return text;
    for (uint32_t i = 0; i < 96; ++i)
    {
        if (uint64_t(address) + i + 1 >= PPC_MEMORY_SIZE)
            break;
        const char c = char(*gears::Memory().Translate<uint8_t>(address + i));
        if (c == 0)
            break;
        text.push_back(c);
    }
    return text;
}
} // namespace

PPC_FUNC(sub_82611E90)
{
    g_createFile.anywhere.fetch_add(1);
    const bool mine = FromLoader(ctx);
    const std::string path = mine ? NarrowGuestString(ctx.r3.u32) : std::string();

    __imp__sub_82611E90(ctx, base);

    if (!mine)
        return;
    const uint64_t n = g_createFile.inLoader.fetch_add(1) + 1;
    if (n <= 3)
        lucent::debug("linker", "loader CreateFile #{}: path {:#x} = '{}' ->"
            " handle {:#x}{}", n, ctx.r4.u32, path, ctx.r3.u32,
            int32_t(ctx.r3.u32) == -1
                ? "  <- INVALID_HANDLE_VALUE, so the loader takes its"
                  " GetLastError branch"
                : "");
}

PPC_FUNC(sub_82612390)
{
    g_readFile.anywhere.fetch_add(1);
    const bool mine = FromLoader(ctx);

    __imp__sub_82612390(ctx, base);

    if (!mine)
        return;
    const uint64_t n = g_readFile.inLoader.fetch_add(1) + 1;
    if (n <= 3)
        lucent::debug("linker", "loader ReadFile #{}: returned {:#x} ({})", n,
            ctx.r3.u32, ctx.r3.u32 != 0
                ? "TRUE, so the loader returns SUCCESS"
                : "FALSE, so the loader returns GetLastError");
}

PPC_FUNC(sub_826124F8)
{
    g_lastError.anywhere.fetch_add(1);
    const bool mine = FromLoader(ctx);

    __imp__sub_826124F8(ctx, base);

    if (!mine)
        return;
    const uint64_t n = g_lastError.inLoader.fetch_add(1) + 1;
    if (n <= 3)
        lucent::debug("linker", "loader GetLastError #{}: returned {:#x}{}", n,
            ctx.r3.u32, ctx.r3.u32 == 0
                ? "  <- ZERO. The loader returns this as its status, so a FAILED"
                  " open is reported to the caller as SUCCESS. That is the lie."
                : "  (non-zero, so the failure propagates honestly)");
}

namespace gears
{
void ReportLoaderThunks()
{
    lucent::debug("linker", "loader thunks: CreateFile {}/{} ReadFile {}/{}"
        " GetLastError {}/{} (inside the loader / anywhere). Two zeros in a pair"
        " means the override never fired at all, not that the call did not happen",
        g_createFile.inLoader.load(), g_createFile.anywhere.load(),
        g_readFile.inLoader.load(), g_readFile.anywhere.load(),
        g_lastError.inLoader.load(), g_lastError.anywhere.load());
}
} // namespace gears

// THE LOADER'S EARLY CALLS, to find where it actually leaves.
//
// The three thunks I hooked first -- on a labelling I took on trust rather than
// verifying -- reported 0 calls from inside the loader against 39, 760 and 8340
// from elsewhere. So those overrides fire and the loader reaches NONE of them,
// which contradicts the disassembly's only route to returning 0. Either the
// labelling is wrong or the loader leaves earlier than the code reads.
//
// These are the calls the loader makes BEFORE any of that, all in
// ppc_recomp.97.cpp and so cross-TU from the caller: 0x826121F0 first, then
// 0x82611900 (the one taking flags 19 and compared against 997), then 0x82612290
// (compared against 0, and the branch that decides whether the path builder runs
// at all). Their return values decide everything, and none of them has been
// observed.
extern "C" PPC_FUNC(__imp__sub_826121F0);
extern "C" PPC_FUNC(__imp__sub_82611900);
extern "C" PPC_FUNC(__imp__sub_82612290);

namespace
{
struct EarlyThunk { std::atomic<uint64_t> inLoader{0}, anywhere{0}; };
EarlyThunk g_first, g_contentCreate, g_overlappedResult;

void ReportEarly(const char* what, EarlyThunk& counts, uint32_t result,
                 const PPCContext& ctx, const char* meaning)
{
    counts.anywhere.fetch_add(1);
    if (!FromLoader(ctx))
        return;
    const uint64_t n = counts.inLoader.fetch_add(1) + 1;
    if (n <= 3)
        lucent::debug("linker", "loader {} #{}: returned {:#x} ({})", what, n,
                      result, meaning);
}
} // namespace

PPC_FUNC(sub_826121F0)
{
    const bool mine = FromLoader(ctx);
    __imp__sub_826121F0(ctx, base);
    if (mine || true)
        ReportEarly("first call (0x826121F0)", g_first, ctx.r3.u32, ctx,
                    "its result is kept in r25 and closed at the end");
}

PPC_FUNC(sub_82611900)
{
    const bool mine = FromLoader(ctx);
    __imp__sub_82611900(ctx, base);
    (void)mine;
    ReportEarly("XamContentCreateEx (0x82611900)", g_contentCreate, ctx.r3.u32,
                ctx, ctx.r3.u32 == 997
                    ? "997 = ERROR_IO_PENDING, which is what the loader requires"
                      " to continue"
                    : "NOT 997, so the loader exits immediately and returns this");
}

PPC_FUNC(sub_82612290)
{
    const bool mine = FromLoader(ctx);
    __imp__sub_82612290(ctx, base);
    (void)mine;
    ReportEarly("XamGetOverlappedResult (0x82612290)", g_overlappedResult,
                ctx.r3.u32, ctx, ctx.r3.u32 == 0
                    ? "ZERO, so the loader proceeds to build a path and open a file"
                    : "non-zero, so the loader skips the open and RETURNS THIS as"
                      " its status");
}

namespace gears
{
void ReportEarlyThunks()
{
    lucent::debug("linker", "loader early thunks: first {}/{},"
        " XamContentCreateEx {}/{}, XamGetOverlappedResult {}/{} (inside the"
        " loader / anywhere). Two zeros in a pair means the override never fired",
        g_first.inLoader.load(), g_first.anywhere.load(),
        g_contentCreate.inLoader.load(), g_contentCreate.anywhere.load(),
        g_overlappedResult.inLoader.load(), g_overlappedResult.anywhere.load());
}
} // namespace gears
