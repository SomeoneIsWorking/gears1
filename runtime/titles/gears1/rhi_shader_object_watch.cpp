#include "rhi_shader_object_watch.h"

#include "gpu_shader_load_watch.h"
#include "guest_write_watch.h"
#include "shader_setter_state.h"

#include <atomic>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears::titles::gears1
{
namespace
{

thread_local std::uint32_t g_knownSetterDepth = 0;
std::atomic<bool> g_reportedEmptySelectedPacket{false};

[[nodiscard]] bool Enabled()
{
    static const bool enabled = lucent::config::flag("WATCH_RHI_PIXEL_SHADER_OBJECT");
    return enabled;
}

} // namespace

bool RhiPixelShaderObjectWatchMayArm(std::uint32_t knownSetterDepth, std::uint32_t shaderObject)
{
    return knownSetterDepth == 0 && shaderObject == 0;
}

bool RhiPixelShaderObjectWatchEnabled()
{
    return Enabled();
}

void PauseRhiPixelShaderObjectWriteWatch()
{
    if (Enabled())
    {
        ++g_knownSetterDepth;
        if (!PauseGuestWriteWatch(GuestWriteWatchOwner::kRhiPixelShaderObject))
            (void)ReportGuestWriteWatch(GuestWriteWatchOwner::kRhiPixelShaderObject, false);
    }
}

void ResumeRhiPixelShaderObjectWriteWatch()
{
    if (Enabled())
    {
        (void)ResumeGuestWriteWatch(GuestWriteWatchOwner::kRhiPixelShaderObject);
        --g_knownSetterDepth;
    }
}

void MaybeArmRhiPixelShaderObjectWriteWatch(std::uint32_t device, std::uint32_t shaderObject)
{
    const bool writeWatch = Enabled();
    const bool transitionWatch = gears::ShaderLoadPacketTransitionWatchEnabled();
    if ((!writeWatch && !transitionWatch) ||
        !RhiPixelShaderObjectWatchMayArm(g_knownSetterDepth, shaderObject) || device == 0)
    {
        return;
    }
    if (transitionWatch)
        gears::ArmShaderLoadPacketTransitionFromZeroMarker();
    if (!writeWatch)
        return;
    const GuestWriteWatchStats stats =
        CurrentGuestWriteWatchStats(GuestWriteWatchOwner::kRhiPixelShaderObject);
    if (stats.aliasPages != 0)
    {
        // A prior unknown write has already disarmed the one-shot watch. The next
        // semantic clear is the earliest ordering boundary that can publish that
        // result without depending on an unrelated packet serializer to run again.
        (void)ReportGuestWriteWatch(GuestWriteWatchOwner::kRhiPixelShaderObject, false);
        return;
    }
    const std::uint32_t offset = ShaderSetterSpecFor(ShaderStage::Pixel).deviceShaderOffset;
    g_reportedEmptySelectedPacket.store(false, std::memory_order_release);
    (void)ArmGuestWriteWatch(GuestWriteWatchOwner::kRhiPixelShaderObject, device + offset, 1);
}

void ReportRhiPixelShaderObjectWriteWatch()
{
    if (!Enabled())
        return;
    const GuestWriteWatchStats stats =
        CurrentGuestWriteWatchStats(GuestWriteWatchOwner::kRhiPixelShaderObject);
    if (stats.armed && stats.targetWrites == 0)
    {
        if (!g_reportedEmptySelectedPacket.exchange(true, std::memory_order_acq_rel))
            lucent::info("hle", "RHI pixel-shader object watch saw no unknown write before its "
                                "first selected packet");
        return;
    }
    (void)ReportGuestWriteWatch(GuestWriteWatchOwner::kRhiPixelShaderObject, false);
}

} // namespace gears::titles::gears1
