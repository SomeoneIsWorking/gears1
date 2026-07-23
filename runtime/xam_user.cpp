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

#include <cstring>

#include <lucent/log.h>

#include <byteswap.h>

#include "input.h"

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
// The profile exists but carries no stored settings, so the title falls back to
// its own defaults -- which is what a freshly created profile produces. The
// buffer-size protocol is still honoured: a null buffer is a size query.
void __imp__XamUserReadProfileSettings(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint32_t bufferSizePtr = ctx.r9.u32;
    const uint32_t buffer = ctx.r10.u32;

    if (buffer == 0)
    {
        // A size query. Reporting the header alone says "no settings" without
        // asking the caller for memory it will find empty.
        Store32(base, bufferSizePtr, 4);
        ctx.r3.u64 = gears::kErrorInsufficientBuffer;
        return;
    }

    // XUSER_READ_PROFILE_SETTING_RESULT begins with the returned setting count.
    Store32(base, buffer, 0);
    lucent::debug("xam", "XamUserReadProfileSettings -> profile has no stored settings");
    ctx.r3.u64 = gears::kErrorSuccess;
}

// DWORD XamUserWriteProfileSettings(...)
//
// There is no storage device to write to, and saying otherwise would have the
// title believe settings persist across runs when they cannot.
void __imp__XamUserWriteProfileSettings(PPCContext& __restrict ctx, uint8_t*)
{
    lucent::debug("xam", "XamUserWriteProfileSettings -> no storage device");
    ctx.r3.u64 = gears::kErrorDeviceNotConnected;
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
void __imp__XamContentGetDeviceState(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::kErrorDeviceNotConnected;
}

void __imp__XamContentGetDeviceData(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::kErrorDeviceNotConnected;
}

void __imp__XamContentCreateEnumerator(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::kErrorDeviceNotConnected;
}

void __imp__XamContentCreateEx(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::kErrorDeviceNotConnected;
}

void __imp__XamContentGetCreator(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::kErrorNotFound;
}

void __imp__XamContentDelete(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::kErrorNotFound;
}

// Closing something that was never opened has nothing to fail at.
void __imp__XamContentClose(PPCContext& __restrict ctx, uint8_t*)
{
    ctx.r3.u64 = gears::kErrorSuccess;
}

// System UI. There is no dashboard overlay to raise. Refusing immediately is
// important: a title told the UI opened would wait for a completion
// notification that nothing can ever post.
#define GEARS_XAM_NO_UI(name)                                    \
    void __imp__##name(PPCContext& __restrict ctx, uint8_t*)     \
    {                                                            \
        lucent::debug("xam", #name " -> no system UI");           \
        ctx.r3.u64 = gears::kErrorAccessDenied;                  \
    }

GEARS_XAM_NO_UI(XamShowAchievementsUI)
GEARS_XAM_NO_UI(XamShowDeviceSelectorUI)
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
    Store32(base, ctx.r7.u32, 0); // items returned
    ctx.r3.u64 = gears::kErrorNoMoreFiles;
}
