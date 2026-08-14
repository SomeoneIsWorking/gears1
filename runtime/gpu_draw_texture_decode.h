#pragma once

#include <cstdint>
#include <vector>

namespace gears::draw
{

enum class TexHostFormat : uint32_t
{
    kUnsupported = 0,
    kR8Unorm,
    kR8G8Unorm,
    kR8G8B8A8Unorm,
    kR5G6B5Pack16,
    kA1R5G5B5Pack16,
    kB4G4R4A4Pack16,
    kA2B10G10R10Pack32,
    kR16Sfloat,
    kR16G16Sfloat,
    kR16G16B16A16Sfloat,
    kR16Unorm,
    kR16G16Unorm,
    kR16G16B16A16Unorm,
    kR32Sfloat,
    kR32G32Sfloat,
    kR32G32B32A32Sfloat,
    kBc1RgbaUnorm,
    kBc2Unorm,
    kBc3Unorm,
    kBc4Unorm,
    kBc5Unorm,
};

struct GuestTextureLevel
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t blocksX = 0;
    uint32_t blocksY = 0;
    uint32_t rowPitchBytes = 0;
    uint64_t dataOffset = 0;
    uint64_t dataSize = 0;
};

struct GuestTexture
{
    uint32_t formatRaw = 0;
    uint32_t baseFormatRaw = 0;
    const char* formatName = "";
    uint32_t dimension = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depthOrArraySize = 1;
    bool tiled = false;
    bool packedMips = false;
    uint32_t mipMin = 0, mipMax = 0;
    uint32_t baseAddress = 0;
    uint32_t mipAddress = 0;
    uint32_t endian = 0;
    uint32_t guestSwizzle = 0;

    TexHostFormat hostFormat = TexHostFormat::kUnsupported;
    uint8_t hostSwizzle[4] = {0, 1, 2, 3};

    // Base and mip storage are disjoint guest spans. Both extents are filled
    // for header-only decoding so cache invalidation covers all authored mips.
    uint32_t baseGuestExtentBytes = 0;
    uint32_t mipGuestExtentBytes = 0;

    uint32_t blockWidth = 1, blockHeight = 1, bytesPerBlock = 4;
    uint32_t layers = 1;
    uint32_t depth3D = 1;
    std::vector<GuestTextureLevel> levels;
    std::vector<uint8_t> data;

    const char* skipReason = nullptr;
};

// Decodes every authored level named by a texture fetch. Header-only decoding
// still reports both guest storage spans for cache invalidation.
bool DecodeGuestTexture(const uint32_t* fetch6, const uint8_t* guestBase,
                        uint64_t guestSize, bool wantData, GuestTexture& out);

} // namespace gears::draw
