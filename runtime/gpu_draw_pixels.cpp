// Bit-level conversions between the guest's number formats and the host's.
// gpu_draw_pixels.h carries the reasoning for each; this file is the arithmetic.

#include "gpu_draw_pixels.h"

#include <bit>
#include <cstring>
#include <fstream>

namespace gears::draw
{

const char* VkStr(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "INCOMPATIBLE_DRIVER";
    default: return "VkResult";
    }
}

std::vector<uint8_t> PackFloatConstants(const uint32_t* regDwords,
    const uint64_t bitmap[4], uint32_t floatCount, uint32_t regBase)
{
    std::vector<uint8_t> out(size_t(std::max(floatCount, 1u)) * 16, 0);
    uint8_t* w = out.data();
    for (uint32_t block = 0; block < 4; ++block) {
        uint64_t entry = bitmap[block];
        while (entry) {
            uint32_t idx = uint32_t(std::countr_zero(entry));
            entry &= ~(uint64_t(1) << idx);
            uint32_t constant = block * 64 + idx;
            std::memcpy(w, &regDwords[regBase + constant * 4], 16);
            w += 16;
        }
    }
    return out;
}

bool FormatSupportsStorage(VkPhysicalDevice physical, VkFormat format)
{
    if (format == VK_FORMAT_UNDEFINED)
        return false;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

float Depth20e4To32(uint32_t f24)
{
    const uint32_t exponent = (f24 >> 20) & 0xF;
    const uint32_t mantissa = f24 & 0xFFFFF;
    uint32_t unbiasedExponent, f32Mantissa;
    if (exponent != 0)
    {
        unbiasedExponent = exponent;
        f32Mantissa = mantissa;
    }
    else if (mantissa != 0)
    {
        // Denormal: normalise the mantissa and pay for it in the exponent.
        const uint32_t msb = 31u - uint32_t(__builtin_clz(mantissa));
        unbiasedExponent = msb - 19u;             // wraps below 19, as intended
        f32Mantissa = mantissa << (20u - msb);
    }
    else
    {
        // Zero in, zero out: -112 cancels the +112 bias below.
        unbiasedExponent = uint32_t(-112);
        f32Mantissa = 0;
    }
    const uint32_t biased = (unbiasedExponent + 112u) & 0xFFu;
    const uint32_t bits = ((f32Mantissa & 0xFFFFFu) | (biased << 20)) << 3;
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

float DepthUnorm24To32(uint32_t d24)
{
    return float(d24 & 0xFFFFFFu) / float(0xFFFFFF);
}

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000) << 16;
    uint32_t exponent = (h >> 10) & 0x1F;
    uint32_t mantissa = h & 0x3FF;
    uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0)
            bits = sign;
        else
        {
            // Subnormal: normalise into a float32 normal.
            exponent = 1;
            while ((mantissa & 0x400) == 0) { mantissa <<= 1; --exponent; }
            mantissa &= 0x3FF;
            bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
        }
    }
    else if (exponent == 0x1F)
        bits = sign | 0x7F800000u | (mantissa << 13); // inf / NaN
    else
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

bool WritePpm(const std::filesystem::path& path, const uint8_t* rgba,
              uint32_t w, uint32_t h)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << "P6\n" << w << ' ' << h << "\n255\n";
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* src = rgba + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x)
        {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        f.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
    return true;
}

} // namespace gears::draw
