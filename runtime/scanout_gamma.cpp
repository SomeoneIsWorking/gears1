#include "scanout_gamma.h"

namespace gears::draw
{

ScanoutGammaLut BuildScanoutGammaLut(const uint32_t *guestRamp)
{
    ScanoutGammaLut lut{};
    if (!guestRamp)
        return lut;

    for (uint32_t i = 0; i < lut.size(); ++i)
    {
        const uint32_t packed = guestRamp[i];
        lut[i].red = ((packed >> 20) & 0x3FFu) >> 2;
        lut[i].green = ((packed >> 10) & 0x3FFu) >> 2;
        lut[i].blue = (packed & 0x3FFu) >> 2;
    }
    return lut;
}

bool ApplyScanoutGamma(std::span<uint8_t> rgba, const ScanoutGammaLut &lut)
{
    if ((rgba.size() & 3u) != 0)
        return false;

    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        const uint8_t red = rgba[i + 0];
        const uint8_t green = rgba[i + 1];
        const uint8_t blue = rgba[i + 2];
        rgba[i + 0] = static_cast<uint8_t>(lut[red].red);
        rgba[i + 1] = static_cast<uint8_t>(lut[green].green);
        rgba[i + 2] = static_cast<uint8_t>(lut[blue].blue);
    }
    return true;
}

} // namespace gears::draw
