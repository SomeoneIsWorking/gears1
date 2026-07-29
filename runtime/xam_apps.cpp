#include "xam_apps.h"

#include <lucent/log.h>

namespace gears
{
namespace
{

constexpr uint32_t kSuccess = 0;
constexpr uint32_t kFail = 0x80004005; // E_FAIL

uint32_t LoadBE32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void StoreBE32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

// ---------------------------------------------------------------------------
// XMP -- the background music player.
//
// There is no system music player here, so the honest answer to every question
// is "the title is in charge of its own audio and nothing is locking it". That
// is a state a real console produces (no music playing, nothing queued), which
// is why it is safe to report.
bool DispatchXmp(uint8_t* guestBase, uint32_t message, uint32_t bufferAddress,
                 uint32_t& status)
{
    switch (message)
    {
    case 0x0007001A: // XMPSetPlaybackController
        // The title asking to take control of playback. It already has it.
        status = kSuccess;
        return true;

    case 0x0007001B:
    {
        // XMPGetPlaybackController. The buffer is a REQUEST structure holding
        // two guest pointers, and the answers go THROUGH them -- writing into
        // the request itself would scribble on the title's own block.
        //   +0 xmp_client, +4 controller pointer, +8 locked pointer
        const uint8_t* request = guestBase + bufferAddress;
        const uint32_t controllerPtr = LoadBE32(request + 4);
        const uint32_t lockedPtr = LoadBE32(request + 8);

        if (controllerPtr != 0)
            StoreBE32(guestBase + controllerPtr, 0); // 0: the title controls it
        if (lockedPtr != 0)
            StoreBE32(guestBase + lockedPtr, 0);     // and it is not locked

        status = kSuccess;
        return true;
    }

    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// XGI -- contexts, properties and presence.
//
// The title reporting what it is doing ("in campaign", "on this map"). On a
// console these feed the dashboard and Live presence. There is nothing to feed
// here, but ACCEPTING them matters: the console accepts them, and a title told
// its own state report failed may retry or take a different path.
bool DispatchXgi(uint32_t message, uint32_t& status)
{
    switch (message)
    {
    case 0x000B0006: // XGIUserSetContext
    case 0x000B0007: // XGIUserSetPropertyEx
    case 0x000B0008: // XGIUserGetProperty
        status = kSuccess;
        return true;

    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// XLiveBase -- the Live client.
//
// This machine is not signed in to Live and never will be, so the useful
// distinction is between the calls that describe the LOCAL state (which can be
// answered) and the ones that need an account (which must fail the way a real
// console fails when signed out).
bool DispatchXLiveBase(uint8_t* guestBase, uint32_t message,
                       uint32_t bufferAddress, uint32_t bufferLength,
                       uint32_t& status)
{
    switch (message)
    {
    case 0x00058004: // XLiveBaseGetLogonId
        // Called at startup; the console returns a logon id in the buffer.
        if (bufferAddress != 0 && bufferLength >= 4)
            StoreBE32(guestBase + bufferAddress, 1);
        status = kSuccess;
        return true;

    case 0x00058006: // XLiveBaseGetNatType
    case 0x00058007: // XOnlineGetServiceInfo
        status = kOnlineNotLoggedOn;
        return true;

    case 0x00058020: // CXLiveFriends::Enumerate
    case 0x00058023: // XMessageGameInviteGetAcceptedInfo
        // There is no friends list and no invite. Failing is what a signed-out
        // console does, and it is what stops the title waiting for a list.
        status = kFail;
        return true;

    case 0x00058037: // XPresenceInitialize
        status = kSuccess;
        return true;

    default:
        return false;
    }
}

} // namespace

bool DispatchXamMessage(uint8_t* guestBase, uint32_t app, uint32_t message,
                        uint32_t bufferAddress, uint32_t bufferLength,
                        uint32_t& status)
{
    bool handled = false;
    switch (app)
    {
    case kXamAppXmp:
        handled = DispatchXmp(guestBase, message, bufferAddress, status);
        break;
    case kXamAppXgi:
        handled = DispatchXgi(message, status);
        break;
    case kXamAppXLiveBase:
        handled = DispatchXLiveBase(guestBase, message, bufferAddress,
                                    bufferLength, status);
        break;
    default:
        break;
    }

    if (handled)
        lucent::debug("xam", "app {:#x} message {:#x} -> {:#x}", app, message,
                      status);
    return handled;
}

} // namespace gears
