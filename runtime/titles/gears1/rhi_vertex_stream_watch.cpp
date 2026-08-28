#include "rhi_vertex_stream_watch.h"

#include "guest_write_watch.h"
#include "rhi_vertex_buffer.h"

#include <lucent/config.h>

namespace gears::gears1
{

namespace
{

constexpr std::uint32_t kObservedSlot = 1;

} // namespace

bool RhiVertexStreamResetWriteWatchEnabled()
{
    static const bool enabled = lucent::config::flag("WATCH_RHI_VERTEX_STREAM_RESET");
    return enabled;
}

void PauseRhiVertexStreamResetWriteWatch(std::uint32_t slot)
{
    if (RhiVertexStreamResetWriteWatchEnabled() && slot == kObservedSlot)
        (void)PauseGuestWriteWatch(GuestWriteWatchOwner::kRhiVertexStreamReset);
}

void MaybeArmRhiVertexStreamResetWriteWatch(std::uint32_t device, std::uint32_t slot)
{
    if (!RhiVertexStreamResetWriteWatchEnabled() || device == 0 || slot != kObservedSlot)
        return;
    const auto owner = GuestWriteWatchOwner::kRhiVertexStreamReset;
    (void)ArmGuestWriteWatch(
        owner, device + kVertexStreamObjectTableOffset + kObservedSlot * sizeof(std::uint32_t), 1);
    (void)ResumeGuestWriteWatch(owner);
}

void ReportRhiVertexStreamResetWriteWatch()
{
    if (RhiVertexStreamResetWriteWatchEnabled())
        (void)ReportGuestWriteWatch(GuestWriteWatchOwner::kRhiVertexStreamReset, false);
}

} // namespace gears::gears1
