#pragma once

#include <cstdint>

namespace gears::gears1
{

[[nodiscard]] bool RhiVertexStreamResetWriteWatchEnabled();
void PauseRhiVertexStreamResetWriteWatch(std::uint32_t slot);
void MaybeArmRhiVertexStreamResetWriteWatch(std::uint32_t device, std::uint32_t slot);
void ReportRhiVertexStreamResetWriteWatch();

} // namespace gears::gears1
