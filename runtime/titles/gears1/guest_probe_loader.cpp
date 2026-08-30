#include "guest_probe_state.h"
#include "import_stub.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_memory.h"

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
        const uint16_t unit = __builtin_bswap16(*gears::Memory().Translate<uint16_t>(at));
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

namespace gears::titles::gears1
{
void NoteLinkerEntry(uint32_t holder)
{
    if (holder == 0 || uint64_t(holder) + 1400 >= PPC_MEMORY_SIZE)
        return;
    const uint32_t loader = ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 0x560));
    const uint32_t nameData = ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 0xcc));
    const uint32_t nameLen = ByteSwap(*gears::Memory().Translate<uint32_t>(holder + 0xd0));
    std::lock_guard<std::mutex> guard(g_linkerMutex);
    LinkerVisit &visit = g_linkerVisits[holder];
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
        lucent::debug("linker",
                      "FArchiveAsync: {} constructed, ZERO with"
                      " ArIsError. CreateLoader's open-failed branch therefore never ran,"
                      " and the create-delete-use-in-one-call reading is DEAD. Blind spot:"
                      " only LOAD_SeekFree packages reach this constructor; the plain"
                      " CreateFileReader path at 0x82496638 does not",
                      built);
    else
    {
        lucent::debug("linker",
                      "FArchiveAsync: {} constructed, {} of them with"
                      " ArIsError set -- i.e. GFileManager->FileSize() returned negative"
                      " and the file was NOT FOUND",
                      built, failed);
        std::lock_guard<std::mutex> guard(g_archiveMutex);
        for (const std::string &name : g_archiveAsyncFailedNames)
            lucent::debug("linker", "  could not open: '{}'", name);
    }

    if (holder == 0 || uint64_t(holder) + 1400 >= PPC_MEMORY_SIZE)
    {
        lucent::debug("linker",
                      "r24 {:#x} is not a readable guest pointer, so"
                      " the ULinkerLoad cannot be dumped",
                      holder);
        return;
    }
    const auto word = [&](uint32_t offset)
    { return ByteSwap(*gears::Memory().Translate<uint32_t>(holder + offset)); };
    const uint32_t nameData = word(0xcc);
    const uint32_t nameLen = word(0xd0);
    lucent::debug("linker",
                  "ULinkerLoad {:#x}: Filename '{}' ({} chars),"
                  " LoadFlags {:#x}, Loader(+1376) {:#x},"
                  " bHasSerializedPackageFileSummary(+1396) {:#x}, ArVer {}",
                  holder, nameLen ? GuestWideString(nameData) : std::string("<empty>"), nameLen,
                  word(0x14c), word(0x560), word(0x574), word(0xe4));

    {
        std::lock_guard<std::mutex> guard(g_linkerMutex);
        const auto it = g_linkerVisits.find(holder);
        if (it == g_linkerVisits.end())
            lucent::debug("linker",
                          "  this ULinkerLoad never entered"
                          " sub_824961D0 through the probe ({} distinct linkers were"
                          " recorded), so the entry state below is unknown -- treat that"
                          " as the PROBE failing, not as a finding",
                          g_linkerVisits.size());
        else
            lucent::debug("linker",
                          "  it entered CreateLoader {} time(s); on the"
                          " most recent entry Loader was {:#x}, which is the {} path. {}",
                          it->second.visits, it->second.loaderAtEntry,
                          it->second.loaderAtEntry ? "MEMO-HIT (creation skipped)" : "CREATE",
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
                      !known  ? "was never seen by the pool probe at all -- so either it did"
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
} // namespace gears::titles::gears1

void gears::titles::gears1::NoteArchiveAsync(uint32_t self, uint32_t nameAddress)
{
    const std::string name = GuestWideString(nameAddress);
    const uint64_t n = g_archiveAsyncBuilt.fetch_add(1) + 1;
    const uint32_t isError = ByteSwap(*gears::Memory().Translate<uint32_t>(self + 0x2c));
    const int32_t fileSize = int32_t(ByteSwap(*gears::Memory().Translate<uint32_t>(self + 0x78)));
    if (isError == 0)
    {
        // A handful of successes, to prove the seam reads the right fields
        // before any conclusion is drawn from a zero failure count.
        if (n <= 4)
            lucent::info("linker",
                         "FArchiveAsync #{} at {:#x} opened '{}'"
                         " ({} bytes)",
                         n, self, name, fileSize);
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
        lucent::debug("linker",
                      "FArchiveAsync #{} at {:#x} FAILED TO OPEN '{}'"
                      " (FileSize {} -> ArIsError). CreateLoader has already memoised it"
                      " at ULinkerLoad+1376 and is about to delete it and carry on"
                      " (failure {} of {} constructions)",
                      n, self, name, fileSize, bad, n);
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

namespace gears::titles::gears1
{
void ReportMapChangeSeams()
{
    lucent::debug("linker",
                  "PrepareMapChange seam: {} call(s); LoadPackageAsync"
                  " seam: {} request(s), {} of them with an EMPTY name.{}",
                  g_prepareMapChanges.load(), g_asyncLoadRequests.load(),
                  g_emptyAsyncLoadRequests.load(),
                  g_asyncLoadRequests.load() == 0
                      ? " ZERO requests means this seam NEVER FIRED -- the async load is"
                        " started somewhere else and nothing below is evidence."
                      : "");
}
} // namespace gears::titles::gears1

PPC_FUNC(sub_82426D98)
{
    const uint64_t n = g_prepareMapChanges.fetch_add(1) + 1;
    const uint32_t array = ctx.r4.u32;
    if (array != 0 && uint64_t(array) + 12 < PPC_MEMORY_SIZE)
    {
        const uint32_t data = ByteSwap(*gears::Memory().Translate<uint32_t>(array));
        const uint32_t count = ByteSwap(*gears::Memory().Translate<uint32_t>(array + 4));
        lucent::info("linker", "PrepareMapChange #{}: {} level name(s) at {:#x}", n, count, data);
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
        lucent::debug("linker",
                      "PrepareMapChange #{}: r4 {:#x} is not a readable"
                      " array pointer, so the level list could not be read",
                      n, array);
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
        lucent::debug("linker",
                      "LoadPackageAsync #{}: name FString at {:#x} ="
                      " data {:#x} len {} -> '{}'{}",
                      n, str, data, length,
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

    const uint32_t index = (out != 0 && uint64_t(out) + 4 < PPC_MEMORY_SIZE)
                               ? ByteSwap(*gears::Memory().Translate<uint32_t>(out))
                               : 0xFFFFFFFFu;

    const uint64_t n = g_fnameFromMapChange.fetch_add(1) + 1;
    // THE ONE LINE A NORMAL RUN SHOULD PRINT. The checkpoint restore is the
    // milestone this whole investigation was about, and a run that silently stops
    // restoring it would otherwise look identical to a run that works. An index
    // of 0 is NAME_None, which is exactly the failure that produced #45.
    if (n == 1)
        lucent::info("linker",
                     "checkpoint restore: the map name resolved to '{}'"
                     " (FName index {:#x}){}",
                     text, index,
                     index == 0 ? "  <- NAME_None, so the restore has REGRESSED: the title"
                                  " will ask for a package called 'None' (catalog #45)"
                                : "");

    lucent::debug("linker",
                  "FName for the map change #{}: string at {:#x} = '{}'"
                  " ({} chars{}), find-mode r5={} r6={} r7={} -> INDEX {:#x}{}",
                  n, stringAddress, text, text.size(),
                  stringAddress == 0 ? ", NULL POINTER"
                  : !readable        ? ", UNREADABLE ADDRESS"
                  : text.empty()     ? ", EMPTY STRING -- this is why the name is None"
                                     : "",
                  ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, index,
                  index == 0 ? "  <- NAME_None, so the package request will be 'None'" : "");
}

namespace gears::titles::gears1
{
// Reported at the abort, so the zero case is never silent.
void ReportMapNameProbe()
{
    lucent::debug("linker",
                  "map-name probe: {} FName construction(s) seen from"
                  " the map-change site out of {} in the whole run. A zero on the left with"
                  " a non-zero on the right means the return-address filter"
                  " (0x821B4620..0x821B5000) is wrong, NOT that the name is fine",
                  g_fnameFromMapChange.load(), g_fnameCalls.load());
}
} // namespace gears::titles::gears1

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

    const auto word = [&](uint32_t address) -> uint32_t
    {
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
        const uint16_t unit = ByteSwap(*gears::Memory().Translate<uint16_t>(0x8209EF7Cu + i * 2));
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
            const uint16_t unit = ByteSwap(*gears::Memory().Translate<uint16_t>(at));
            if (unit == 0)
                break;
            supplied.push_back(unit < 0x80 ? char(unit) : '?');
        }
    }

    lucent::debug("linker",
                  "carrier gate #{}: object {:#x}, out-param {:#x}"
                  " data {:#x} count {} = '{}' ({} path), fallback literal '{}'",
                  n, object, param, suppliedData, paramCount, supplied,
                  paramCount <= 1 ? "literal" : "other", literal);
    lucent::debug("linker",
                  "  object+420 before: data {:#x} count {} -> after:"
                  " data {:#x} count {}; RETURNED {:#x}",
                  beforeData, int32_t(beforeCount), afterData, int32_t(afterCount), result);
    lucent::debug("linker", "  the carrier meanwhile holds {} bytes at {:#x}",
                  int32_t(word(kCarrier + 4)), word(kCarrier));
    lucent::debug("linker", "  => {}",
                  result != 0
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

    lucent::debug("linker",
                  "save-device checkpoint load #{}: RETURNED {:#x} ({});"
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
extern "C" PPC_FUNC(__imp__sub_82611E90); // CreateFile
extern "C" PPC_FUNC(__imp__sub_82612390); // ReadFile
extern "C" PPC_FUNC(__imp__sub_826124F8); // GetLastError

namespace
{
constexpr uint32_t kLoaderStart = 0x821B6800;
constexpr uint32_t kLoaderEnd = 0x821B6980;

struct ThunkCounts
{
    std::atomic<uint64_t> inLoader{0}, anywhere{0};
};
ThunkCounts g_createFile, g_readFile, g_lastError;

bool FromLoader(const PPCContext &ctx)
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
        lucent::debug("linker",
                      "loader CreateFile #{}: path {:#x} = '{}' ->"
                      " handle {:#x}{}",
                      n, ctx.r4.u32, path, ctx.r3.u32,
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
        lucent::debug("linker", "loader ReadFile #{}: returned {:#x} ({})", n, ctx.r3.u32,
                      ctx.r3.u32 != 0 ? "TRUE, so the loader returns SUCCESS"
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
        lucent::debug("linker", "loader GetLastError #{}: returned {:#x}{}", n, ctx.r3.u32,
                      ctx.r3.u32 == 0
                          ? "  <- ZERO. The loader returns this as its status, so a FAILED"
                            " open is reported to the caller as SUCCESS. That is the lie."
                          : "  (non-zero, so the failure propagates honestly)");
}

namespace gears::titles::gears1
{
void ReportLoaderThunks()
{
    lucent::debug("linker",
                  "loader thunks: CreateFile {}/{} ReadFile {}/{}"
                  " GetLastError {}/{} (inside the loader / anywhere). Two zeros in a pair"
                  " means the override never fired at all, not that the call did not happen",
                  g_createFile.inLoader.load(), g_createFile.anywhere.load(),
                  g_readFile.inLoader.load(), g_readFile.anywhere.load(),
                  g_lastError.inLoader.load(), g_lastError.anywhere.load());
}
} // namespace gears::titles::gears1

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
struct EarlyThunk
{
    std::atomic<uint64_t> inLoader{0}, anywhere{0};
};
EarlyThunk g_first, g_contentCreate, g_overlappedResult;

void ReportEarly(const char *what, EarlyThunk &counts, uint32_t result, const PPCContext &ctx,
                 const char *meaning)
{
    counts.anywhere.fetch_add(1);
    if (!FromLoader(ctx))
        return;
    const uint64_t n = counts.inLoader.fetch_add(1) + 1;
    if (n <= 3)
        lucent::debug("linker", "loader {} #{}: returned {:#x} ({})", what, n, result, meaning);
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
    ReportEarly("XamContentCreateEx (0x82611900)", g_contentCreate, ctx.r3.u32, ctx,
                ctx.r3.u32 == 997 ? "997 = ERROR_IO_PENDING, which is what the loader requires"
                                    " to continue"
                                  : "NOT 997, so the loader exits immediately and returns this");
}

PPC_FUNC(sub_82612290)
{
    const bool mine = FromLoader(ctx);
    __imp__sub_82612290(ctx, base);
    (void)mine;
    ReportEarly("XamGetOverlappedResult (0x82612290)", g_overlappedResult, ctx.r3.u32, ctx,
                ctx.r3.u32 == 0 ? "ZERO, so the loader proceeds to build a path and open a file"
                                : "non-zero, so the loader skips the open and RETURNS THIS as"
                                  " its status");
}

namespace gears::titles::gears1
{
void ReportEarlyThunks()
{
    lucent::debug("linker",
                  "loader early thunks: first {}/{},"
                  " XamContentCreateEx {}/{}, XamGetOverlappedResult {}/{} (inside the"
                  " loader / anywhere). Two zeros in a pair means the override never fired",
                  g_first.inLoader.load(), g_first.anywhere.load(), g_contentCreate.inLoader.load(),
                  g_contentCreate.anywhere.load(), g_overlappedResult.inLoader.load(),
                  g_overlappedResult.anywhere.load());
}
} // namespace gears::titles::gears1
