// Tests for XamUserGetName at the guest seam.
//
// The console's rule is the whole content of this function, and it is not the
// obvious one: Length counts the terminator, and a buffer too small for the
// gamertag is TRUNCATED into rather than refused. This runtime had it as a
// refusal, which is invisible from inside -- every call still returned a
// documented status. What it broke was outside: Gears' sub_821B5DD8 memsets a
// 48-byte global, copies "save:\" into it, calls
//
//     XamUserGetName(userIndex, global + 6, 4)
//
// and hands the whole string on as a save path. With a refusal, nothing was
// written and every save path stayed the bare device root "save:\".
//
// The statuses come from Xenia's XamUserGetName_entry
// (extern/xenia/src/xenia/kernel/xam/xam_user.cc), which transcribes XAM:
// an index past the last slot is a bad parameter, an empty slot has its first
// byte cleared and reports no such user, and anything else succeeds.

#include <cstdio>
#include <cstring>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"

#include "kernel_status.h"
#include "user_profile.h"

// The guest seam, so the copy is exercised through the code the title calls
// rather than through a copy of it.
void __imp__XamUserGetName(PPCContext& __restrict ctx, uint8_t* base);

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

constexpr uint32_t kSignedInSlot = 0;
constexpr uint32_t kEmptySlot = 1;
constexpr uint32_t kFirstSlotPastTheEnd = 4;
constexpr uint32_t kBuffer = 0x40;

std::vector<uint8_t> g_memory;

uint32_t Call(uint32_t userIndex, uint32_t buffer, uint32_t length)
{
    PPCContext ctx{};
    ctx.r3.u64 = userIndex;
    ctx.r4.u64 = buffer;
    ctx.r5.u64 = length;
    __imp__XamUserGetName(ctx, g_memory.data());
    return uint32_t(ctx.r3.u64);
}

const char* NameAt(uint32_t address)
{
    return reinterpret_cast<const char*>(g_memory.data() + address);
}

void Reset()
{
    g_memory.assign(0x200, 0xCD);
}

// The call Gears makes, in the shape it makes it: the name is appended to
// "save:\" in place and the result has to be a usable path.
void TestGearsSavePathCall()
{
    Reset();
    const char kDeviceRoot[] = "save:\\";
    const uint32_t pathAddress = kBuffer;
    std::memcpy(g_memory.data() + pathAddress, kDeviceRoot, sizeof(kDeviceRoot));

    const uint32_t nameAddress = pathAddress + uint32_t(sizeof(kDeviceRoot)) - 1;
    Check(Call(kSignedInSlot, nameAddress, 4) == gears::kErrorSuccess,
        "save path: a four-byte buffer succeeds, it is not too small");
    Check(std::strlen(NameAt(pathAddress)) == sizeof(kDeviceRoot) - 1 + 3,
        "save path: the device root grew by three characters of the name");
    Check(std::strcmp(NameAt(pathAddress), "save:\\") != 0,
        "save path: the path is no longer the bare device root");

    char expected[32];
    std::snprintf(expected, sizeof(expected), "%s%.3s", kDeviceRoot,
                  gears::kGamertag);
    Check(std::strcmp(NameAt(pathAddress), expected) == 0,
        "save path: device root followed by the truncated gamertag");
}

// The seam has to hand the copy the caller's Length unchanged; a buffer one
// byte longer than the name is the first that holds all of it.
void TestSeamPassesLengthThrough()
{
    const uint32_t nameBytes = uint32_t(std::strlen(gears::kGamertag)) + 1;

    Reset();
    Check(Call(kSignedInSlot, kBuffer, nameBytes) == gears::kErrorSuccess,
        "seam: a buffer of name plus terminator succeeds");
    Check(std::strcmp(NameAt(kBuffer), gears::kGamertag) == 0,
        "seam: ... and holds the whole gamertag");

    Reset();
    Check(Call(kSignedInSlot, kBuffer, nameBytes - 1) == gears::kErrorSuccess,
        "seam: one byte short still succeeds");
    Check(std::strlen(NameAt(kBuffer)) == nameBytes - 2,
        "seam: ... having dropped exactly one character");
    Check(g_memory[kBuffer + nameBytes - 1] == 0xCD,
        "seam: ... and wrote nothing past the caller's buffer");
}

// An index that names a slot the console does not have is a bad argument. An
// index that names an empty slot is a missing user, and XAM clears the first
// byte of the buffer for it -- so the caller reads an empty name rather than
// whatever the buffer held before.
void TestSlotStatuses()
{
    Reset();
    Check(Call(kFirstSlotPastTheEnd, kBuffer, 16) == gears::kErrorInvalidParameter,
        "slots: an index past the last slot is a bad parameter");
    Check(g_memory[kBuffer] == 0xCD,
        "slots: ... and nothing is written for it");

    Reset();
    Check(Call(kEmptySlot, kBuffer, 16) == gears::kErrorNoSuchUser,
        "slots: a slot with no profile reports no such user");
    Check(g_memory[kBuffer] == 0x00,
        "slots: ... with the first byte cleared");
    Check(g_memory[kBuffer + 1] == 0xCD,
        "slots: ... and only the first byte");
}

} // namespace

int main()
{
    TestGearsSavePathCall();
    TestSeamPassesLengthThrough();
    TestSlotStatuses();

    if (g_failures == 0)
    {
        printf("all XamUserGetName tests passed\n");
        return 0;
    }
    printf("%d XamUserGetName test(s) FAILED\n", g_failures);
    return 1;
}
