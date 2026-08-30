#pragma once

#include <cstdint>

namespace gears::titles::gears1
{

[[nodiscard]] constexpr bool RhiTextureDescriptorWatchMayArm(std::uint32_t knownSetterDepth)
{
    return knownSetterDepth == 1;
}

void PauseRhiTextureDescriptorWriteWatch();
void ResumeRhiTextureDescriptorWriteWatch();
void MaybeArmRhiTextureDescriptorWriteWatch(std::uint32_t device, std::uint32_t slot);
void ReportRhiTextureDescriptorWriteWatch();

} // namespace gears::titles::gears1
