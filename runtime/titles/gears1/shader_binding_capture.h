#pragma once

#include "rhi_semantic_stream.h"
#include "shader_setter_state.h"

#include <cstdint>
#include <span>

namespace gears::titles::gears1
{

[[nodiscard]] RhiBindingStateEvidence
CaptureShaderBinding(ShaderStage stage, std::uint32_t device,
                     std::span<const RhiShaderModuleEvidence> shaderModules = {},
                     bool shaderModulesPresent = false);

} // namespace gears::titles::gears1
