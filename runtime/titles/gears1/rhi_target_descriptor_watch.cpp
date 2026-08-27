#include "rhi_target_descriptor_watch.h"

#include "guest_write_watch.h"

#include <lucent/config.h>

namespace gears::gears1
{

bool RhiTargetDescriptorWriteWatchEnabled()
{
    static const bool enabled = lucent::config::flag("WATCH_RHI_TARGET_DESCRIPTOR");
    return enabled;
}

void PauseRhiTargetDescriptorWriteWatch()
{
    if (RhiTargetDescriptorWriteWatchEnabled())
        (void)PauseGuestWriteWatch(GuestWriteWatchOwner::kRhiTargetDescriptor);
}

void MaybeArmRhiTargetDescriptorWriteWatch(std::uint32_t device, std::uint32_t slot)
{
    constexpr std::uint32_t kColorDescriptorBase = 0x2804;
    constexpr std::uint32_t kSlotZero = 0;
    if (!RhiTargetDescriptorWriteWatchEnabled() || device == 0 || slot != kSlotZero)
        return;
    const auto owner = GuestWriteWatchOwner::kRhiTargetDescriptor;
    (void)ArmGuestWriteWatch(owner, device + kColorDescriptorBase, 1);
    (void)ResumeGuestWriteWatch(owner);
}

void ReportRhiTargetDescriptorWriteWatch()
{
    if (RhiTargetDescriptorWriteWatchEnabled())
        (void)ReportGuestWriteWatch(GuestWriteWatchOwner::kRhiTargetDescriptor, false);
}

} // namespace gears::gears1
