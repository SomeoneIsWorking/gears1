#pragma once

#include "rhi_semantic_stream.h"

#include <array>
#include <cstdint>

namespace gears::gears1
{

struct ResolveCallState
{
    std::uint32_t flags = 0;
    std::uint32_t sourceObject = 0;
    std::uint32_t destinationObject = 0;
    std::array<std::uint32_t, 6> destinationDescriptor{};
    std::array<std::int32_t, 4> sourceRectangle{};
    std::array<std::int32_t, 2> destinationPoint{};
    std::uint32_t bytesPerBlock = 0;
};

[[nodiscard]] RhiSemanticResolve DecodeResolveCall(const ResolveCallState &call);

} // namespace gears::gears1
