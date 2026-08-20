#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace gears::draw
{

// One std430-compatible LUT entry. The guest packs ten-bit blue, green and red
// channels into DC_LUT_30_COLOR; scan-out to an eight-bit host image uses the
// upper eight bits of each component.
struct alignas(16) ScanoutGammaEntry
{
    uint32_t red = 0;
    uint32_t green = 0;
    uint32_t blue = 0;
    uint32_t padding = 0;
};

using ScanoutGammaLut = std::array<ScanoutGammaEntry, 256>;

ScanoutGammaLut BuildScanoutGammaLut(const uint32_t *guestRamp);

// Applies the same LUT used by the GPU scan-out pass to tightly packed RGBA8
// host pixels. Returns false for a malformed, non-pixel-aligned byte span.
bool ApplyScanoutGamma(std::span<uint8_t> rgba, const ScanoutGammaLut &lut);

} // namespace gears::draw
