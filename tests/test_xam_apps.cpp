// Tests for the Xam application message dispatch.
//
// XMsgInProcessCall and XMsgStartIORequest are how a title reaches the system
// applications -- the media player, the gaming/presence service, the Live
// client. Every message used to be refused, which is honest but leaves the
// title talking to a system that is not there. These tests pin the messages
// Gears actually sends, identified against Xenia's app dispatch.
//
// The contract that matters: a HANDLED message and an UNKNOWN one must be
// distinguishable, so an unimplemented service is still visible rather than
// being papered over with a blanket success.

#include <cstdio>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"

#include "xam_apps.h"

PPCFuncMapping PPCFuncMappings[] = { { 0, nullptr } };

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

uint32_t ReadBE32(const uint8_t* p)
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

constexpr uint32_t kBufferAddress = 0x1000;

void TestUnknownIsNotSwallowed()
{
    std::vector<uint8_t> memory(0x4000, 0);
    uint32_t status = 0;

    Check(!gears::DispatchXamMessage(memory.data(), 0x99, 0x12345,
                                     kBufferAddress, 4, status),
        "dispatch: an unknown app is reported unhandled");
    Check(!gears::DispatchXamMessage(memory.data(), gears::kXamAppXgi, 0xDEAD,
                                     kBufferAddress, 4, status),
        "dispatch: an unknown message on a known app is reported unhandled");
}

// XLiveBaseGetLogonId. Called at startup; the console returns a logon id in
// the buffer.
void TestXLiveBaseLogonId()
{
    std::vector<uint8_t> memory(0x4000, 0xCD);
    uint32_t status = 0xFFFFFFFF;

    Check(gears::DispatchXamMessage(memory.data(), gears::kXamAppXLiveBase,
                                    0x00058004, kBufferAddress, 4, status),
        "xlivebase: the logon id message is handled");
    Check(status == 0, "xlivebase: and succeeds");
    Check(ReadBE32(memory.data() + kBufferAddress) == 1,
        "xlivebase: a logon id is written into the buffer");
}

// Anything that genuinely needs Live must fail as NOT LOGGED ON rather than
// succeed emptily -- a title told it is online will wait for data that never
// comes.
void TestXLiveBaseOfflineServices()
{
    std::vector<uint8_t> memory(0x4000, 0);
    uint32_t status = 0;

    Check(gears::DispatchXamMessage(memory.data(), gears::kXamAppXLiveBase,
                                    0x00058006, kBufferAddress, 4, status),
        "xlivebase: NAT type is handled");
    Check(status == gears::kOnlineNotLoggedOn,
        "xlivebase: NAT type reports not logged on");
}

// XGIUserSetContext / SetProperty: the title reporting what it is doing. There
// is nothing to do with the value, but refusing it is wrong -- the console
// accepts it.
void TestXgiContextAccepted()
{
    std::vector<uint8_t> memory(0x4000, 0);
    uint32_t status = 0xFFFFFFFF;

    Check(gears::DispatchXamMessage(memory.data(), gears::kXamAppXgi,
                                    0x000B0006, kBufferAddress, 12, status),
        "xgi: set context is handled");
    Check(status == 0, "xgi: set context succeeds");
}

// XMPGetPlaybackController. The buffer is a request structure carrying two
// GUEST POINTERS, and the results are written THROUGH them -- not into the
// request. Getting that wrong writes over the title's request block.
void TestXmpPlaybackControllerWritesThroughPointers()
{
    std::vector<uint8_t> memory(0x4000, 0);
    constexpr uint32_t kControllerAddress = 0x2000;
    constexpr uint32_t kLockedAddress = 0x2100;

    uint8_t* request = memory.data() + kBufferAddress;
    StoreBE32(request + 0, 0);                   // xmp_client = Game
    StoreBE32(request + 4, kControllerAddress);
    StoreBE32(request + 8, kLockedAddress);

    uint32_t status = 0xFFFFFFFF;
    Check(gears::DispatchXamMessage(memory.data(), gears::kXamAppXmp,
                                    0x0007001B, kBufferAddress, 12, status),
        "xmp: get playback controller is handled");
    Check(status == 0, "xmp: and succeeds");

    // The title controls playback (there is no system music player here), and
    // it is not locked.
    Check(ReadBE32(memory.data() + kControllerAddress) == 0,
        "xmp: the title is reported as the playback controller");
    Check(ReadBE32(memory.data() + kLockedAddress) == 0,
        "xmp: playback control is not locked");

    // The request structure itself must be untouched.
    Check(ReadBE32(request + 4) == kControllerAddress,
        "xmp: the request block is not overwritten");
}

} // namespace

int main()
{
    TestUnknownIsNotSwallowed();
    TestXLiveBaseLogonId();
    TestXLiveBaseOfflineServices();
    TestXgiContextAccepted();
    TestXmpPlaybackControllerWritesThroughPointers();

    if (g_failures == 0)
    {
        printf("all xam app tests passed\n");
        return 0;
    }
    printf("%d xam app test(s) FAILED\n", g_failures);
    return 1;
}
