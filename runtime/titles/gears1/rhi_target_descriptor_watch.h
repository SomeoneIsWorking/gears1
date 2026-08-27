#pragma once

#include <cstdint>

namespace gears::gears1
{

[[nodiscard]] bool RhiTargetDescriptorWriteWatchEnabled();
void PauseRhiTargetDescriptorWriteWatch();
void MaybeArmRhiTargetDescriptorWriteWatch(std::uint32_t device, std::uint32_t slot);
void ReportRhiTargetDescriptorWriteWatch();

} // namespace gears::gears1
