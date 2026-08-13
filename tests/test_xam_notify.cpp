#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "ppc_config.h"
#include "ppc_context.h"

#include "xam_notify.h"

void __imp__XamNotifyCreateListener(PPCContext& ctx, uint8_t* base);
void __imp__XNotifyGetNext(PPCContext& ctx, uint8_t* base);

PPCFuncMapping PPCFuncMappings[] = {{0, nullptr}};

namespace
{
int failures = 0;
std::vector<uint8_t> memory(0x100, 0xCD);

uint32_t Swap(uint32_t value)
{
    return __builtin_bswap32(value);
}

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", message);
        ++failures;
    }
}

uint32_t Create(uint64_t mask, uint32_t maxVersion = 10)
{
    PPCContext ctx{};
    ctx.r3.u64 = uint32_t(mask >> 32);
    ctx.r4.u64 = uint32_t(mask);
    ctx.r5.u64 = maxVersion;
    __imp__XamNotifyCreateListener(ctx, memory.data());
    return ctx.r3.u32;
}

bool Next(uint32_t handle, uint32_t match, uint32_t& id, uint32_t& param)
{
    constexpr uint32_t idAddress = 0x20;
    constexpr uint32_t paramAddress = 0x24;
    PPCContext ctx{};
    ctx.r3.u64 = handle;
    ctx.r4.u64 = match;
    ctx.r5.u64 = idAddress;
    ctx.r6.u64 = paramAddress;
    __imp__XNotifyGetNext(ctx, memory.data());
    id = Swap(*reinterpret_cast<uint32_t*>(memory.data() + idAddress));
    param = Swap(*reinterpret_cast<uint32_t*>(memory.data() + paramAddress));
    return ctx.r3.u32 != 0;
}

void TestSystemUIOpenAndClose()
{
    gears::ResetNotificationsForTest();
    const uint32_t listener = Create(1); // kXNotifySystem
    gears::BroadcastNotification(gears::kXNotificationSystemUI, 1);
    gears::ScheduleNotification(gears::kXNotificationSystemUI, 0,
        std::chrono::milliseconds(20));

    uint32_t id = 0, param = 0;
    Check(Next(listener, gears::kXNotificationSystemUI, id, param),
        "system listener receives UI-open notification");
    Check(id == gears::kXNotificationSystemUI && param == 1,
        "UI-open carries the system UI id and active parameter");
    Check(!Next(listener, 0, id, param),
        "delayed UI-close is not visible before its deadline");

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Check(Next(listener, 0, id, param),
        "system listener receives delayed UI-close notification");
    Check(id == gears::kXNotificationSystemUI && param == 0,
        "UI-close carries inactive parameter");
    Check(!Next(listener, 0, id, param),
        "both notifications are consumed exactly once");
}

void TestWrongAreaRefuses()
{
    gears::ResetNotificationsForTest();
    const uint32_t liveOnly = Create(1ull << 1); // kXNotifyLive
    gears::BroadcastNotification(gears::kXNotificationSystemUI, 1);
    uint32_t id = 0, param = 0;
    Check(!Next(liveOnly, 0, id, param),
        "live-only listener rejects a system notification");
}
}

int main()
{
    TestSystemUIOpenAndClose();
    TestWrongAreaRefuses();
    if (failures == 0)
    {
        std::printf("all Xam notification tests passed\n");
        return 0;
    }
    std::printf("%d Xam notification test(s) FAILED\n", failures);
    return 1;
}
