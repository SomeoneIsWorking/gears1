#include "guest_write_watch.h"

#include <cassert>

#include "guest_memory.h"
#include "fault_report.h"
#include "ppc_config.h"
#include "ppc_context.h"

PPCFuncMapping PPCFuncMappings[] = {{0, nullptr}};

int main()
{
    using gears::GuestWriteWatchContains;

    assert(GuestWriteWatchContains(0x1000, 4, 0x1000));
    assert(GuestWriteWatchContains(0x1000, 4, 0x1003));
    assert(!GuestWriteWatchContains(0x1000, 4, 0x0FFF));
    assert(!GuestWriteWatchContains(0x1000, 4, 0x1004));
    assert(!GuestWriteWatchContains(0x1000, 0, 0x1000));

    gears::DrawPacketWatchSelector selector;
    assert(!selector.Observe(499, 1, 500, 2));
    assert(!selector.Observe(500, 0, 500, 2));
    assert(!selector.Observe(500, 1, 500, 2));
    assert(!selector.Observe(500, 1, 500, 2));
    assert(!selector.Observe(501, 1, 500, 2));
    assert(!selector.Observe(501, 1, 500, 2));
    assert(selector.Observe(501, 1, 500, 2));
    assert(!selector.Observe(502, 1, 500, 2));

    gears::GuestMemory memory;
    assert(memory.Reserve());
    gears::SetMemory(memory);
    gears::InstallFaultReporter();

    constexpr uint32_t kTarget = 0x00123000;
    assert(gears::ArmGuestWriteWatch(gears::GuestWriteWatchOwner::kDrawPacket, kTarget, 1));
    gears::GuestWriteWatchStats stats =
        gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.armed);
    assert(stats.aliasPages == gears::GuestMemory::kAliasCount);

    *memory.Translate<volatile uint32_t>(0xA0123004) = 0xBAD0BAD0;
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.targetWrites == 0);
    assert(stats.otherPageWrites == 1);

    *memory.Translate<volatile uint32_t>(0xC0123000) = 0xC001C0DE;
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(!stats.armed);
    assert(stats.targetWrites == 1);
    assert(stats.otherPageWrites == 1);
    assert(gears::ReportGuestWriteWatch(gears::GuestWriteWatchOwner::kDrawPacket, false));

    memory.Release();
    return 0;
}
