#include "rhi_texture_descriptor_watch.h"

#include "guest_write_watch.h"
#include "rhi_texture_state.h"

#include <algorithm>

#include <lucent/config.h>

namespace gears::titles::gears1
{
namespace
{

thread_local std::uint32_t g_knownSetterDepth = 0;

[[nodiscard]] bool Enabled()
{
    static const bool enabled = lucent::config::flag("WATCH_RHI_TEXTURE_DESCRIPTOR");
    return enabled;
}

[[nodiscard]] std::uint32_t SelectedSlot()
{
    static const auto slot = static_cast<std::uint32_t>(
        std::max<long>(0, lucent::config::number("WATCH_RHI_TEXTURE_DESCRIPTOR_SLOT", 0)));
    return slot;
}

[[nodiscard]] std::uint32_t SelectedDword()
{
    static const auto dword = static_cast<std::uint32_t>(
        std::max<long>(0, lucent::config::number("WATCH_RHI_TEXTURE_DESCRIPTOR_DWORD", 3)));
    return dword;
}

} // namespace

void PauseRhiTextureDescriptorWriteWatch()
{
    if (Enabled())
    {
        ++g_knownSetterDepth;
        (void)PauseGuestWriteWatch(GuestWriteWatchOwner::kRhiTextureDescriptor);
    }
}

void ResumeRhiTextureDescriptorWriteWatch()
{
    if (Enabled())
    {
        (void)ResumeGuestWriteWatch(GuestWriteWatchOwner::kRhiTextureDescriptor);
        --g_knownSetterDepth;
    }
}

void MaybeArmRhiTextureDescriptorWriteWatch(std::uint32_t device, std::uint32_t slot)
{
    const std::uint32_t dword = SelectedDword();
    if (!Enabled() || !RhiTextureDescriptorWatchMayArm(g_knownSetterDepth) || device == 0 ||
        slot != SelectedSlot() || slot >= kRhiTextureSlotCount ||
        dword >= kRhiTextureDescriptorDwords)
        return;

    constexpr std::uint32_t kDeviceRegisterShadowOffset = 0x400;
    const std::uint32_t descriptorDword =
        slot * static_cast<std::uint32_t>(kRhiTextureDescriptorDwords) + dword;
    const auto owner = GuestWriteWatchOwner::kRhiTextureDescriptor;
    (void)ArmGuestWriteWatch(
        owner, device + kDeviceRegisterShadowOffset + descriptorDword * sizeof(std::uint32_t), 1);
}

void ReportRhiTextureDescriptorWriteWatch()
{
    if (Enabled())
        (void)ReportGuestWriteWatch(GuestWriteWatchOwner::kRhiTextureDescriptor, false);
}

} // namespace gears::titles::gears1
