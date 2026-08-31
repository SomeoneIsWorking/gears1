#pragma once

#include <cstdint>

namespace gears::titles::gears1
{

[[nodiscard]] bool RhiPixelShaderObjectWatchMayArm(std::uint32_t knownSetterDepth,
                                                   std::uint32_t shaderObject);
[[nodiscard]] bool RhiPixelShaderObjectWatchEnabled();
void PauseRhiPixelShaderObjectWriteWatch();
void ResumeRhiPixelShaderObjectWriteWatch();
void MaybeArmRhiPixelShaderObjectWriteWatch(std::uint32_t device, std::uint32_t shaderObject);
void ReportRhiPixelShaderObjectWriteWatch();

} // namespace gears::titles::gears1
