// Profiles, storage, controllers and system UI, overridden natively.
//
// These are OS seams: the guest side of them is a thin shim onto services the
// console provides and this runtime does not, so reimplementing the title's
// code buys nothing. What matters is that every answer describes ONE coherent
// console, because titles cross-check them -- a profile that is signed in but
// owns no storage device and has no controller is a real machine; a profile
// that is signed in to Live while the network reports no link is not.
//
// The machine described here: one local profile in slot 0, signed in offline,
// no Live, no storage device attached, no controller connected, and no system
// UI available. Every one of those is a state hardware produces, so the title
// is obliged to handle each of them.
#include "import_stub.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <vector>
#include "xam_content.h"
#include "xam_overlapped.h"
#include <mutex>
#include <memory>
#include <map>

#include <byteswap.h>
#include <lucent/log.h>

#include "guest_filesystem.h"
#include "input.h"
#include "user_profile.h"

namespace
{
constexpr uint32_t kLocalUser = 0;

// 0 = not signed in, 1 = signed in locally, 2 = signed in to Live. Local is
// the only choice consistent with the networking layer reporting no link.
constexpr uint32_t kSignedInLocally = 1;

// Offline profiles use XUIDs in the reserved offline range, so this cannot be
// mistaken for a real Live account.
constexpr uint64_t kOfflineXuid = 0xE000000000000001ull;

constexpr char kProfileName[] = "Player";

void Store32(uint8_t* base, uint32_t address, uint32_t value)
{
    if (address != 0)
        *reinterpret_cast<uint32_t*>(base + address) = ByteSwap(value);
}

void Store64(uint8_t* base, uint32_t address, uint64_t value)
{
    if (address != 0)
        *reinterpret_cast<uint64_t*>(base + address) = ByteSwap(value);
}

uint32_t Load32(const uint8_t* base, uint32_t address)
{
    if (address == 0)
        return 0;
    return ByteSwap(*reinterpret_cast<const uint32_t*>(base + address));
}

void Store16(uint8_t* base, uint32_t address, uint16_t value)
{
    if (address != 0)
        *reinterpret_cast<uint16_t*>(base + address) = ByteSwap(value);
}

// X_INPUT_GAMEPAD, 12 bytes: buttons (BE u16), the two triggers as bytes, then
// four big-endian signed thumb axes.
void StoreGamepad(uint8_t* base, uint32_t address, const gears::PadState& pad)
{
    Store16(base, address + 0, pad.buttons);
    base[address + 2] = pad.leftTrigger;
    base[address + 3] = pad.rightTrigger;
    Store16(base, address + 4, uint16_t(pad.thumbLX));
    Store16(base, address + 6, uint16_t(pad.thumbLY));
    Store16(base, address + 8, uint16_t(pad.thumbRX));
    Store16(base, address + 10, uint16_t(pad.thumbRY));
}

constexpr uint32_t kInputStateBytes = 16;        // packet number + gamepad
constexpr uint32_t kInputCapabilitiesBytes = 20; // type/sub_type/flags + gamepad + vibration

// XamInput's user index carries a "any user" marker in its high bits, which
// pins to slot 0 -- one local profile is the only user this runtime has.
constexpr uint32_t kUserIndexAny = 0x000000FF;
constexpr uint32_t kMaxUsers = 4;

bool IsLocalUser(uint32_t index)
{
    return index == kLocalUser;
}

// A live content enumeration. The console hands back a handle and the title
// pulls items through it in batches, so the position has to live somewhere
// across calls.
struct ContentEnumerator
{
    std::vector<gears::ContentEntry> items;
    size_t next = 0;
    uint32_t itemsPerCall = 1;
};

std::mutex g_enumeratorsMutex;
std::map<uint32_t, std::unique_ptr<ContentEnumerator>> g_enumerators;
uint32_t g_nextEnumerator = 0xE0000000;

// The file name out of an XCONTENT_DATA, reduced to something that can safely
// become a path. This string comes from the guest and is about to name a
// directory on the player's disk, so anything that is not plainly a file name
// is dropped rather than escaped.
std::string SafeContentFileName(const uint8_t* base, uint32_t contentDataPtr)
{
    const char* raw = reinterpret_cast<const char*>(
        base + contentDataPtr + 0x108);
    std::string name;
    for (size_t i = 0; i < gears::kContentFileNameBytes && raw[i]; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        name.push_back(std::isalnum(c) || c == '_' || c == '-' ? char(c) : '_');
    }
    return name;
}
} // namespace

// DWORD XamUserGetSigninState(DWORD UserIndex)
void __imp__XamUserGetSigninState(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = IsLocalUser(ctx.r3.u32) ? kSignedInLocally : 0;
}

// DWORD XamUserGetXUID(DWORD UserIndex, DWORD Type, PXUID Xuid)
void __imp__XamUserGetXUID(PPCContext& __restrict ctx, uint8_t* base)
{
    if (!IsLocalUser(ctx.r3.u32))
    {
        ctx.r3.u64 = gears::kErrorNoSuchUser;
        return;
    }

    Store64(base, ctx.r5.u32, kOfflineXuid);
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamUserGetName(DWORD UserIndex, LPSTR Buffer, DWORD Length)
void __imp__XamUserGetName(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t buffer = ctx.r4.u32;
    const uint32_t length = ctx.r5.u32;

    if (!IsLocalUser(ctx.r3.u32))
    {
        ctx.r3.u64 = gears::kErrorNoSuchUser;
        return;
    }

    if (buffer == 0 || length <= sizeof(kProfileName) - 1)
    {
        ctx.r3.u64 = gears::kErrorInsufficientBuffer;
        return;
    }

    std::memcpy(base + buffer, kProfileName, sizeof(kProfileName));
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamUserCheckPrivilege(DWORD UserIndex, DWORD Type, PBOOL Result)
//
// Every privilege this matters for is an online one, and there is no Live
// session to grant them. Denying is the same answer an offline profile gets.
void __imp__XamUserCheckPrivilege(PPCContext& __restrict ctx, uint8_t* base)
{
    Store32(base, ctx.r5.u32, 0);
    ctx.r3.u64 = IsLocalUser(ctx.r3.u32) ? gears::kErrorSuccess : gears::kErrorNoSuchUser;
}

// DWORD XamUserAreUsersFriends(DWORD UserIndex, PDWORD, DWORD, PBOOL Result, PVOID)
void __imp__XamUserAreUsersFriends(PPCContext& __restrict ctx, uint8_t* base)
{
    Store32(base, ctx.r6.u32, 0);
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamUserReadProfileSettings(DWORD TitleId, DWORD UserIndex, DWORD XuidCount,
//                                  PXUID Xuids, DWORD SettingCount, PDWORD SettingIds,
//                                  PDWORD BufferSize, PVOID Buffer, PVOID Overlapped)
//
// Backed by a real store now (user_profile.cpp), persisted next to the saves.
// The buffer protocol is the console's: a caller with no buffer, or too small a
// one, is told the size it needs; a caller with room gets a result whose
// per-setting source says whether the value was ever set, which is how the
// title tells "no value" from "the value is zero".
void __imp__XamUserReadProfileSettings(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t settingCount = ctx.r7.u32;
    const uint32_t settingIdsPtr = ctx.r8.u32;
    const uint32_t bufferSizePtr = ctx.r9.u32;
    const uint32_t buffer = ctx.r10.u32;

    // The console bounds this, and a count outside it would size a buffer from
    // a number the title never meant as one.
    if (settingCount < 1 || settingCount > 32 || bufferSizePtr == 0 ||
        settingIdsPtr == 0)
    {
        ctx.r3.u64 = gears::kErrorInvalidParameter;
        return;
    }

    std::vector<uint32_t> ids(settingCount);
    for (uint32_t i = 0; i < settingCount; ++i)
        ids[i] = Load32(base, settingIdsPtr + i * 4);

    const uint32_t needed =
        gears::ProfileReadBufferSize(ids.data(), settingCount);
    const uint32_t bufferSize = Load32(base, bufferSizePtr);

    if (buffer == 0 || bufferSize < needed)
    {
        Store32(base, bufferSizePtr, needed);
        ctx.r3.u64 = gears::kErrorInsufficientBuffer;
        return;
    }

    gears::WriteProfileReadResult(base, buffer, bufferSize, gears::Profile(),
                                  ids.data(), settingCount);
    lucent::debug("xam", "read {} profile setting(s)", settingCount);
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamUserWriteProfileSettings(DWORD UserIndex, DWORD SettingCount,
//                                   const XUSER_PROFILE_SETTING* Settings,
//                                   PVOID Overlapped)
//
// Settings now have somewhere to go: they are stored and written to disk, so a
// player's choices survive the run. Refusing this was what made the profile
// read-only in practice.
void __imp__XamUserWriteProfileSettings(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t settingCount = ctx.r4.u32;
    const uint32_t settings = ctx.r5.u32;

    if (settingCount == 0 || settings == 0)
    {
        ctx.r3.u64 = gears::kErrorSuccess;
        return;
    }

    for (uint32_t i = 0; i < settingCount; ++i)
    {
        const uint32_t record = settings + i * gears::kProfileSettingSize;
        const uint32_t id = Load32(base, record + 16);
        const auto type = gears::UserDataType(base[record + 24]);

        switch (type)
        {
        case gears::UserDataType::Int32:
        case gears::UserDataType::Context:
        case gears::UserDataType::Float:
            gears::Profile().SetInt32(id, int32_t(Load32(base, record + 32)));
            break;

        case gears::UserDataType::Binary:
        {
            const uint32_t size = Load32(base, record + 32);
            const uint32_t data = Load32(base, record + 36);
            if (data != 0 && size != 0 && size <= 0x1000)
                gears::Profile().SetBinary(id,
                    std::vector<uint8_t>(base + data, base + data + size));
            break;
        }

        default:
            // Storing a type this runtime cannot round-trip would lose it
            // silently on the next read; saying so keeps the gap visible.
            lucent::debug("xam", "profile setting {:#x} has unsupported type {}",
                          id, uint32_t(type));
            break;
        }
    }

    gears::Profile().Save(gears::Files().SaveDirectory() / "profile.bin");
    lucent::debug("xam", "wrote {} profile setting(s)", settingCount);
    ctx.r3.u64 = gears::kErrorSuccess;
}

void __imp__XamUserCreateStatsEnumerator(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::kErrorNotFound; // leaderboards are a Live service
}

// Controllers. The pad is reported CONNECTED only when a host input source
// actually exists -- a gamepad or keyboard behind the window, or a scripted
// run. Reporting a connected pad with no source behind it would read as a
// player who never presses anything, which leaves a title waiting at its
// "press start" prompt for ever; reporting disconnected is a state hardware
// really produces and every title handles.

// DWORD XamInputGetState(DWORD UserIndex, DWORD Flags, PXINPUT_STATE State)
void __imp__XamInputGetState(PPCContext& __restrict ctx, uint8_t* base)
{
    // A scripted run has no event loop of its own; the guest's own poll is what
    // advances it, which is also what makes the timings line up with the frames
    // the guest is actually producing.
    gears::UpdateScriptedInput();

    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t stateAddress = ctx.r5.u32;

    // The console zeroes the structure before anything else, so a title that
    // ignores the return code still reads a defined state.
    if (stateAddress != 0)
        std::memset(base + stateAddress, 0, kInputStateBytes);

    if ((userIndex & kUserIndexAny) != kUserIndexAny && userIndex >= kMaxUsers)
    {
        ctx.r3.u64 = gears::kErrorDeviceNotConnected;
        return;
    }
    const uint32_t slot = (userIndex & kUserIndexAny) == kUserIndexAny ? kLocalUser : userIndex;
    if (!IsLocalUser(slot) || !gears::PadConnected())
    {
        ctx.r3.u64 = gears::kErrorDeviceNotConnected;
        return;
    }

    // Titles call this with a null pointer as a "is anything plugged in" query.
    if (stateAddress != 0)
    {
        uint32_t packet = 0;
        const gears::PadState pad = gears::CurrentPad(packet);
        Store32(base, stateAddress, packet);
        StoreGamepad(base, stateAddress + 4, pad);
    }
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamInputGetCapabilities(DWORD UserIndex, DWORD Flags, PXINPUT_CAPABILITIES Caps)
void __imp__XamInputGetCapabilities(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t userIndex = ctx.r3.u32;
    const uint32_t capsAddress = ctx.r5.u32;
    const uint32_t slot = (userIndex & kUserIndexAny) == kUserIndexAny ? kLocalUser : userIndex;
    if (!IsLocalUser(slot) || !gears::PadConnected())
    {
        ctx.r3.u64 = gears::kErrorDeviceNotConnected;
        return;
    }
    if (capsAddress != 0)
    {
        std::memset(base + capsAddress, 0, kInputCapabilitiesBytes);
        base[capsAddress + 0] = 1; // XINPUT_DEVTYPE_GAMEPAD
        base[capsAddress + 1] = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
        // The gamepad field of the capabilities is a MASK of what the device
        // can report, not a reading: every button, both triggers, both sticks.
        gears::PadState mask;
        mask.buttons = 0xFFFF;
        mask.leftTrigger = mask.rightTrigger = 0xFF;
        mask.thumbLX = mask.thumbLY = mask.thumbRX = mask.thumbRY = int16_t(0xFFC0);
        StoreGamepad(base, capsAddress + 4, mask);
        Store16(base, capsAddress + 16, 0xFFFF); // left motor range
        Store16(base, capsAddress + 18, 0xFFFF); // right motor range
    }
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamInputSetState(DWORD UserIndex, DWORD Unknown, PXINPUT_VIBRATION Vibration)
// Accepted and dropped: there is no motor to drive, and failing the call would
// be a lie about a pad we have just reported as connected and capable.
void __imp__XamInputSetState(PPCContext& __restrict ctx, uint8_t*)
{
    const uint32_t slot = (ctx.r3.u32 & kUserIndexAny) == kUserIndexAny ? kLocalUser : ctx.r3.u32;
    ctx.r3.u64 = (IsLocalUser(slot) && gears::PadConnected())
        ? gears::kErrorSuccess
        : gears::kErrorDeviceNotConnected;
}

// Storage. No memory unit or hard disc is attached, so there is no content to
// enumerate, create or read.
// The console's storage device, which on a PC is a directory.
//
// Every one of these used to answer "no device connected", which is why the
// title opened a "no storage device" dialog on every boot and could not save.
// A PC always has somewhere to put a save file, so reporting no device was
// describing a machine this port does not run on.
namespace
{
// ONE source of truth for which device this is. It was duplicated here and in
// xam_content.h, both spelling 1 -- and a title that enumerates content on one
// device then opens it on another finds nothing, so the two drifting apart
// would be a silent and very confusing failure.
constexpr uint32_t kDeviceId = gears::kContentDeviceId;

// XDEVICE_DATA: id, type, then two 64-bit byte counts and a friendly name.
constexpr uint32_t kDeviceTypeHdd = 1;
} // namespace

void __imp__XamContentGetDeviceState(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t deviceId = ctx.r3.u32;
    const uint32_t statePtr = ctx.r4.u32;
    if (deviceId != kDeviceId && deviceId != 0)
    {
        ctx.r3.u64 = gears::kErrorDeviceNotConnected;
        return;
    }
    if (statePtr != 0)
        *reinterpret_cast<uint32_t*>(base + statePtr) = 0; // no error
    ctx.r3.u64 = gears::kErrorSuccess;
}

void __imp__XamContentGetDeviceData(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t deviceId = ctx.r3.u32;
    const uint32_t dataPtr = ctx.r4.u32;
    if ((deviceId != kDeviceId && deviceId != 0) || dataPtr == 0)
    {
        ctx.r3.u64 = gears::kErrorDeviceNotConnected;
        return;
    }

    // CAPPED TO CONSOLE SCALE, and the cap is the point.
    //
    // Reporting the host's real capacity looked more honest and immediately
    // broke the title: with a PC disk's terabytes in this field it tried to
    // allocate 0x9f000000 bytes -- 2.6 GB -- and failed, right after mounting
    // its save. The title sizes something from what the device claims, and a
    // number no console could ever return is a number it was never written to
    // handle. 512 MB is the console's own Memory Unit, which is both plausible
    // and far more space than a save needs.
    //
    // Real free space is still consulted, so a genuinely full disk is reported
    // as full rather than as a cheerful lie.
    constexpr uint64_t kDeviceBytes = 512ull << 20;
    std::error_code ec;
    const std::filesystem::space_info space =
        std::filesystem::space(gears::Files().SaveDirectory(), ec);
    const uint64_t total = kDeviceBytes;
    const uint64_t free = ec ? kDeviceBytes : std::min<uint64_t>(kDeviceBytes, space.available);

    uint8_t* d = base + dataPtr;
    *reinterpret_cast<uint32_t*>(d + 0) = ByteSwap(kDeviceId);
    *reinterpret_cast<uint32_t*>(d + 4) = ByteSwap(kDeviceTypeHdd);
    *reinterpret_cast<uint64_t*>(d + 8) = ByteSwap(total);
    *reinterpret_cast<uint64_t*>(d + 16) = ByteSwap(free);
    // A UTF-16 name, which is what the console's dashboard would show.
    static const char16_t kName[] = u"PC Storage";
    for (size_t i = 0; i < std::size(kName); ++i)
        *reinterpret_cast<uint16_t*>(d + 24 + i * 2) = ByteSwap(uint16_t(kName[i]));

    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamContentCreateEnumerator(DWORD UserIndex, DWORD DeviceId,
//                                   DWORD ContentType, DWORD ContentFlags,
//                                   DWORD ItemCount, PDWORD BufferSize,
//                                   PHANDLE Handle)
//
// Walks the player's saves. This used to report no storage device, which meant
// a title could never discover a save it had written -- the write path existed
// and nothing could find its results.
void __imp__XamContentCreateEnumerator(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t contentType = ctx.r5.u32;
    const uint32_t itemCount = ctx.r7.u32;
    const uint32_t bufferSizePtr = ctx.r8.u32;
    const uint32_t handlePtr = ctx.r9.u32;

    if (handlePtr == 0 || itemCount == 0)
    {
        ctx.r3.u64 = gears::kErrorInvalidParameter;
        return;
    }

    auto enumerator = std::make_unique<ContentEnumerator>();
    enumerator->items = gears::EnumerateContent(gears::Files().SaveDirectory(),
                                                contentType);
    enumerator->itemsPerCall = itemCount;

    // The caller sizes its buffer from this, so it must be what a single call
    // can return -- not the whole enumeration.
    Store32(base, bufferSizePtr, itemCount * gears::kContentDataSize);

    uint32_t handle;
    {
        std::lock_guard<std::mutex> guard(g_enumeratorsMutex);
        handle = g_nextEnumerator;
        g_nextEnumerator += 4;
        g_enumerators[handle] = std::move(enumerator);
    }
    Store32(base, handlePtr, handle);

    lucent::debug("xam", "content enumerator {:#x}: {} item(s) of type {:#x}",
                  handle, g_enumerators[handle]->items.size(), contentType);
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamContentCreateEx(DWORD userIndex, LPCSTR rootName,
//                          const XCONTENT_DATA* data, DWORD flags, ...)
//
// The title names a root ("SAVE"), hands over the content it wants, and then
// opens files as "SAVE:\...". So creating content is, on a PC, making a
// directory and mounting it under that name -- after which the ordinary file
// path does the rest.
void __imp__XamContentCreateEx(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t rootNamePtr = ctx.r4.u32;
    const uint32_t contentDataPtr = ctx.r5.u32;
    const uint32_t flags = ctx.r6.u32;
    // XamContentCreateEx(user, root, data, flags, disposition, licenseMask,
    //                    overlapped) -- the async block is the last argument.
    const uint32_t overlapped = ctx.r9.u32;

    if (rootNamePtr == 0)
    {
        ctx.r3.u64 = gears::kErrorInvalidParameter;
        return;
    }
    const std::string rootName(reinterpret_cast<const char*>(base + rootNamePtr));

    // XCONTENT_DATA is device id, content type, then a UTF-16 display name and
    // an ASCII file name. The file name is what distinguishes one save from
    // another, so it is what the directory is called.
    std::string fileName = "savedata";
    if (contentDataPtr != 0)
    {
        const char* raw = reinterpret_cast<const char*>(base + contentDataPtr + 8 + 128 * 2);
        std::string candidate;
        for (size_t i = 0; i < 42 && raw[i]; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(raw[i]);
            // Anything that is not plainly a filename is dropped rather than
            // escaped: this string becomes a path on the user's disk.
            candidate.push_back(std::isalnum(c) || c == '_' || c == '-' ? char(c) : '_');
        }
        if (!candidate.empty())
            fileName = candidate;
    }

    const std::filesystem::path dir = gears::Files().SaveDirectory() / fileName;

    // Content exists when it HOLDS a save -- see xam_content.cpp. Testing for
    // the directory is wrong, because the runtime creates it itself on the
    // first write under the mount.
    const bool existed = gears::ContentExists(dir);

    // THE DISPOSITION IS THE CONSOLE'S, not ours -- see xam_content.h. The title
    // passes flags 0x13, whose low nibble is 3 (OPEN_EXISTING); the previous
    // table called 2 OPEN_EXISTING, so that request matched nothing and was
    // answered as a creation.
    const gears::ContentDecision decision =
        gears::DecideContentCreate(flags, existed);
    lucent::info("xam", "XamContentCreateEx('{}') flags {:#x} (disposition {}),"
        " content {} -> {}", fileName, flags, flags & 0xF,
        existed ? "exists" : "absent",
        decision == gears::ContentDecision::Open ? "open"
        : decision == gears::ContentDecision::Create ? "create"
        : decision == gears::ContentDecision::NotFound ? "NOT FOUND"
        : decision == gears::ContentDecision::AlreadyExists ? "already exists"
        : "invalid");

    uint32_t status = gears::kErrorSuccess;
    switch (decision)
    {
    case gears::ContentDecision::NotFound:
        lucent::debug("xam", "XamContentCreateEx('{}') -> no such content", fileName);
        status = gears::kErrorPathNotFound;
        break;
    case gears::ContentDecision::AlreadyExists:
        status = gears::kErrorAlreadyExists;
        break;
    case gears::ContentDecision::Invalid:
        status = gears::kErrorInvalidParameter;
        break;
    case gears::ContentDecision::Open:
    case gears::ContentDecision::Create:
        gears::Files().Mount(rootName, dir);
        lucent::info("xam", "content '{}' mounted as '{}:' ({})", fileName, rootName,
                     decision == gears::ContentDecision::Open ? "existing" : "new");
        break;
    }

    // THE CALLER WAITS ON THE OVERLAPPED, AND CHECKS FOR IO_PENDING FIRST.
    // Measured at the call site (ppc_recomp.6.cpp around 0x821B6884):
    //     bl XamContentCreateEx ; cmplwi cr6,r30,997 ; bne cr6,<skip everything>
    // The title proceeds ONLY when this returns ERROR_IO_PENDING, then reads the
    // real result out of the overlapped. Returning plain success made it branch
    // away and skip the entire load, which is why a save was never read back.
    gears::CompleteOverlapped(base, overlapped, status);
    ctx.r3.u64 = overlapped != 0 ? gears::kErrorIoPending : status;
}

// DWORD XamContentGetCreator(DWORD UserIndex, const XCONTENT_DATA* Data,
//                            PBOOL IsCreator, PXUID Creator, PVOID Overlapped)
//
// Everything on this machine was created by its one local profile, so the
// player always owns their own saves -- which is what lets a title offer to
// overwrite or delete them.
void __imp__XamContentGetCreator(PPCContext& __restrict ctx, uint8_t* base)
{
    Store32(base, ctx.r5.u32, 1); // the local user is the creator
    Store64(base, ctx.r6.u32, kOfflineXuid);
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamContentDelete(DWORD UserIndex, const XCONTENT_DATA* Data,
//                        PVOID Overlapped)
//
// A player deleting a save from inside the game. This removes the directory
// the content lives in, which is the same thing the console does to a package.
void __imp__XamContentDelete(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t contentDataPtr = ctx.r4.u32;
    if (contentDataPtr == 0)
    {
        ctx.r3.u64 = gears::kErrorInvalidParameter;
        return;
    }

    const std::string fileName = SafeContentFileName(base, contentDataPtr);
    if (fileName.empty())
    {
        ctx.r3.u64 = gears::kErrorInvalidParameter;
        return;
    }

    const std::filesystem::path dir = gears::Files().SaveDirectory() / fileName;
    if (!gears::ContentExists(dir))
    {
        ctx.r3.u64 = gears::kErrorNotFound;
        return;
    }

    std::error_code ec;
    const auto removed = std::filesystem::remove_all(dir, ec);
    if (ec)
    {
        lucent::warn("xam", "could not delete content '{}': {}", fileName,
                     ec.message());
        ctx.r3.u64 = gears::kErrorAccessDenied;
        return;
    }

    lucent::info("xam", "deleted content '{}' ({} file(s))", fileName, removed);
    ctx.r3.u64 = gears::kErrorSuccess;
}

// Closing content unmounts its root, so a later open of the same name cannot
// reach the previous save's directory by accident.
void __imp__XamContentClose(PPCContext& __restrict ctx, uint8_t* base)
{
    if (const uint32_t rootNamePtr = ctx.r3.u32; rootNamePtr != 0)
        gears::Files().Unmount(reinterpret_cast<const char*>(base + rootNamePtr));
    ctx.r3.u64 = gears::kErrorSuccess;
}

// System UI. There is no dashboard overlay to raise. Refusing immediately is
// important: a title told the UI opened would wait for a completion
// notification that nothing can ever post.
// WARN, not debug. A refused system dialog changes what the title does next,
// and the refusal is invisible at the point where the consequence shows up --
// XamShowDeviceSelectorUI sat here quietly while the title, unable to obtain a
// storage device, failed sixty seconds later and four subsystems away. Once
// each, so a title that polls one cannot drown the log.
#define GEARS_XAM_NO_UI(name)                                            \
    void __imp__##name(PPCContext& __restrict ctx, uint8_t*)             \
    {                                                                    \
        static std::atomic<bool> reported{false};                        \
        if (!reported.exchange(true))                                    \
            lucent::warn("xam", #name " refused: this runtime has no"    \
                " system UI. If the title misbehaves after this, that"   \
                " refusal is a candidate for why");                      \
        ctx.r3.u64 = gears::kErrorAccessDenied;                          \
    }

// DWORD XamShowDeviceSelectorUI(DWORD UserIndex, DWORD ContentType,
//                                DWORD ContentFlags, ULONGLONG TotalRequested,
//                                PDWORD DeviceId, PXOVERLAPPED Overlapped)
//
// The one system dialog this runtime CAN answer, and the one it must. Asking a
// player which storage device to use is a question a PC does not have: there is
// exactly one place saves go. So the selector selects it and completes, rather
// than being refused like the dialogs that genuinely have no answer here.
//
// Refusing it was not harmless. A title that cannot obtain a device has no
// destination for a save, and carries on with a save context it never
// established.
void __imp__XamShowDeviceSelectorUI(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t deviceIdPtr = ctx.r7.u32;
    const uint32_t overlapped = ctx.r8.u32;

    if (deviceIdPtr == 0)
    {
        gears::CompleteOverlapped(base, overlapped, gears::kErrorInvalidParameter);
        ctx.r3.u64 = gears::kErrorInvalidParameter;
        return;
    }

    Store32(base, deviceIdPtr, gears::kContentDeviceId);
    lucent::info("xam", "storage device selected automatically: this machine has"
        " one place for saves, so there is nothing to ask");

    // Asynchronous on the console: a caller waiting on the overlapped learns
    // the outcome only through it.
    gears::CompleteOverlapped(base, overlapped, gears::kErrorSuccess);
    ctx.r3.u64 = overlapped != 0 ? gears::kErrorIoPending : gears::kErrorSuccess;
}

GEARS_XAM_NO_UI(XamShowAchievementsUI)
GEARS_XAM_NO_UI(XamShowDirtyDiscErrorUI)
GEARS_XAM_NO_UI(XamShowFriendRequestUI)
GEARS_XAM_NO_UI(XamShowFriendsUI)
GEARS_XAM_NO_UI(XamShowGameInviteUI)
GEARS_XAM_NO_UI(XamShowGamerCardUIForXUID)
GEARS_XAM_NO_UI(XamShowKeyboardUI)
GEARS_XAM_NO_UI(XamShowMarketplaceUI)
GEARS_XAM_NO_UI(XamShowMessageBoxUIEx)
GEARS_XAM_NO_UI(XamShowMessagesUI)
GEARS_XAM_NO_UI(XamShowPlayerReviewUI)
GEARS_XAM_NO_UI(XamShowPlayersUI)
GEARS_XAM_NO_UI(XamShowSigninUI)

// DWORD XamEnumerate(HANDLE Enumerator, DWORD Flags, PVOID Buffer, DWORD Length,
//                    PDWORD ItemsReturned, PVOID Overlapped)
//
// The counterpart to XamContentCreateEnumerator, which reports no storage
// device. With nothing attached there is nothing to walk, and "no more items"
// is how an exhausted enumeration ends normally -- so a caller that loops until
// it sees this terminates on the first pass rather than on an error it may not
// check for.
void __imp__XamEnumerate(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t buffer = ctx.r5.u32;
    const uint32_t length = ctx.r6.u32;
    const uint32_t itemsReturnedPtr = ctx.r7.u32;

    std::lock_guard<std::mutex> guard(g_enumeratorsMutex);
    const auto it = g_enumerators.find(handle);
    if (it == g_enumerators.end())
    {
        Store32(base, itemsReturnedPtr, 0);
        ctx.r3.u64 = gears::kErrorInvalidHandle;
        return;
    }

    ContentEnumerator& enumerator = *it->second;

    // "No more items" is how an exhausted enumeration ends normally, so a
    // caller looping until it sees this terminates rather than treating the end
    // as an error.
    if (enumerator.next >= enumerator.items.size())
    {
        Store32(base, itemsReturnedPtr, 0);
        ctx.r3.u64 = gears::kErrorNoMoreFiles;
        return;
    }

    uint32_t written = 0;
    while (written < enumerator.itemsPerCall &&
           enumerator.next < enumerator.items.size() &&
           (written + 1) * gears::kContentDataSize <= length)
    {
        gears::WriteContentData(base, buffer + written * gears::kContentDataSize,
                                enumerator.items[enumerator.next]);
        ++enumerator.next;
        ++written;
    }

    Store32(base, itemsReturnedPtr, written);
    lucent::debug("xam", "enumerator {:#x} returned {} item(s)", handle, written);
    ctx.r3.u64 = written != 0 ? gears::kErrorSuccess : gears::kErrorNoMoreFiles;
}
