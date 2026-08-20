#include <array>
#include <cstdint>
#include <cstdio>

#include "scanout_gamma.h"

namespace
{

int failures = 0;

void Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

} // namespace

int main()
{
    std::array<uint32_t, 256> guest{};
    for (uint32_t i = 0; i < guest.size(); ++i)
    {
        const uint32_t red = (i * 4u + 3u) & 0x3FFu;
        const uint32_t green = (1023u - i * 4u) & 0x3FFu;
        const uint32_t blue = (i * 2u) & 0x3FFu;
        guest[i] = blue | (green << 10) | (red << 20);
    }

    const gears::draw::ScanoutGammaLut lut = gears::draw::BuildScanoutGammaLut(guest.data());
    Check(lut[4].red == 4, "red comes from packed bits 20..29");
    Check(lut[4].green == 251, "green comes from packed bits 10..19");
    Check(lut[4].blue == 2, "blue comes from packed bits 0..9");

    std::array<uint8_t, 8> pixels{4, 5, 6, 7, 200, 100, 50, 33};
    Check(gears::draw::ApplyScanoutGamma(pixels, lut), "a whole RGBA span is accepted");
    Check(pixels[0] == lut[4].red && pixels[1] == lut[5].green && pixels[2] == lut[6].blue,
          "each input channel indexes its matching LUT component");
    Check(pixels[3] == 7 && pixels[7] == 33, "alpha passes through unchanged");

    std::array<uint8_t, 3> malformed{};
    Check(!gears::draw::ApplyScanoutGamma(malformed, lut), "a partial pixel is refused");

    if (failures == 0)
        std::puts("scan-out gamma tests passed");
    return failures == 0 ? 0 : 1;
}
