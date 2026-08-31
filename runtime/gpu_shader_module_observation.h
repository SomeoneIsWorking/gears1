#pragma once

#include "rhi_semantic_stream.h"

#include <cstdint>
#include <map>
#include <vector>

namespace gears
{

struct GpuCapturedShader
{
    std::uint32_t type = 0;
    std::uint32_t dwords = 0;
    std::uint32_t address = 0;
    std::uint64_t loads = 0;
    bool immediate = false;
    std::vector<std::uint8_t> ucode;
};

void ObserveGpuShaderModulesForDraw(std::uint32_t packetGuestAddress, std::uint64_t vertexHash,
                                    std::uint64_t pixelHash,
                                    const std::map<std::uint64_t, GpuCapturedShader> &shaders);

} // namespace gears
