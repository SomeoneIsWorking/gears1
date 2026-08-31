#include "guest_write_watch.h"
#include "gpu_shader_load_watch.h"
#include "titles/gears1/rhi_texture_descriptor_watch.h"
#include "titles/gears1/rhi_shader_object_watch.h"

#include <cassert>
#include <thread>

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
    assert(gears::GuestWriteWatchImageAddress(0xE962F1, 0) == 0xE962F1);
    assert(gears::GuestWriteWatchImageAddress(0x7F00E962F1, 0x7F00000000) == 0xE962F1);
    assert(gears::GuestWriteWatchImageAddress(0x1000, 0x2000) == 0x1000);
    assert(gears::ParseGuestWriteWatchAddress("820e40b8") == 0x820E40B8U);
    assert(gears::ParseGuestWriteWatchAddress("0X820e40b8") == 0x820E40B8U);
    assert(!gears::ParseGuestWriteWatchAddress("0x"));
    assert(!gears::ParseGuestWriteWatchAddress("not-an-address"));
    assert(!gears::ParseGuestWriteWatchAddress("100000000"));
    assert(gears::ParseShaderLoadWatchHash("63c971f5e9d59913") == 0x63C971F5E9D59913ULL);
    assert(gears::ParseShaderLoadWatchHash("0X63c971f5e9d59913") == 0x63C971F5E9D59913ULL);
    assert(!gears::ParseShaderLoadWatchHash("not-a-hash"));
    assert(!gears::ParseShaderLoadWatchHash("0x"));
    assert(gears::ShaderLoadWatchCopyCoversTarget(0xA0001000, 16, 0xC000100C));
    assert(!gears::ShaderLoadWatchCopyCoversTarget(0x001000, 16, 0x001010));
    assert(!gears::ShaderLoadWatchCopyCoversTarget(0x001000, 0, 0x001000));
    assert(gears::titles::gears1::RhiTextureDescriptorWatchMayArm(1));
    assert(!gears::titles::gears1::RhiTextureDescriptorWatchMayArm(0));
    assert(!gears::titles::gears1::RhiTextureDescriptorWatchMayArm(2));
    assert(gears::titles::gears1::RhiPixelShaderObjectWatchMayArm(0, 0));
    assert(!gears::titles::gears1::RhiPixelShaderObjectWatchMayArm(1, 0));
    assert(!gears::titles::gears1::RhiPixelShaderObjectWatchMayArm(0, 1));

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
    assert(gears::ArmGuestWriteWatch(gears::GuestWriteWatchOwner::kDrawPacket, kTarget, 2));
    gears::GuestWriteWatchStats stats =
        gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.armed);
    assert(stats.aliasPages == gears::GuestMemory::kAliasCount);

    *memory.Translate<volatile uint32_t>(0xA0123004) = 0xBAD0BAD0;
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.targetWrites == 0);
    assert(stats.otherPageWrites == 1);

    assert(gears::PauseGuestWriteWatch(gears::GuestWriteWatchOwner::kDrawPacket));
    assert(gears::PauseGuestWriteWatch(gears::GuestWriteWatchOwner::kDrawPacket));
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.armed);
    std::thread unrelatedWriter([&memory]
                                { *memory.Translate<volatile uint32_t>(0xC0123004) = 0x51504A4C; });
    unrelatedWriter.join();
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.otherPageWrites == 2);
    *memory.Translate<volatile uint32_t>(0xC0123000) = 0x51504A4D;
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.targetWrites == 0);
    assert(gears::ResumeGuestWriteWatch(gears::GuestWriteWatchOwner::kDrawPacket));
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.armed);
    *memory.Translate<volatile uint32_t>(0xC0123000) = 0x51504A4E;
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.targetWrites == 0);
    assert(gears::ResumeGuestWriteWatch(gears::GuestWriteWatchOwner::kDrawPacket));

    *memory.Translate<volatile uint32_t>(0xC0123000) = 0xC001C0DE;
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(stats.armed);
    assert(stats.targetWrites == 1);
    assert(stats.otherPageWrites == 2);
    *memory.Translate<volatile uint32_t>(0xC0123000) = 0xC001D00D;
    stats = gears::CurrentGuestWriteWatchStats(gears::GuestWriteWatchOwner::kDrawPacket);
    assert(!stats.armed);
    assert(stats.targetWrites == 2);
    assert(!gears::ResumeGuestWriteWatch(gears::GuestWriteWatchOwner::kDrawPacket));
    assert(gears::ReportGuestWriteWatch(gears::GuestWriteWatchOwner::kDrawPacket, false));

    memory.Release();
    return 0;
}
