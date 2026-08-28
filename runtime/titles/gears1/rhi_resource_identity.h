#pragma once

#include "rhi_semantic_stream.h"

#include <cstdint>

namespace gears::titles::gears1
{

// The common resource-wrapper fields are decoded here once for all Gears 1
// binding adapters. Native owners consume the resulting identity only; they
// do not infer the title's guest layout.
[[nodiscard]] RhiResourceIdentityEvidence CaptureRhiResourceIdentity(std::uint32_t object);

} // namespace gears::titles::gears1
