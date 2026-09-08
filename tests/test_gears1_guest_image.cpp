#include "gears1_guest_image.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

void Put16(std::vector<std::byte> &bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::byte>(value);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}

void Put32(std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value)
{
    for (std::size_t index = 0; index < 4U; ++index)
    {
        bytes[offset + index] = static_cast<std::byte>(value >> static_cast<unsigned>(index * 8U));
    }
}

std::vector<std::byte> SyntheticNormalizedImage()
{
    std::vector<std::byte> image(0x400U);
    image[0] = std::byte{'M'};
    image[1] = std::byte{'Z'};
    Put32(image, 0x3cU, 0x80U);
    Put32(image, 0x80U, 0x00004550U);
    Put16(image, 0x84U, 0x01f2U);
    Put16(image, 0x86U, 1U);
    Put16(image, 0x94U, 224U);
    Put16(image, 0x98U, 0x10bU);
    Put32(image, 0xa8U, 0x1000U);
    Put32(image, 0xb4U, 0x82000000U);
    Put32(image, 0xb8U, 0x1000U);
    Put32(image, 0xd0U, 0x2000U);
    Put32(image, 0xd4U, 0x200U);
    constexpr std::size_t section = 0x178U;
    image[section] = std::byte{'.'};
    image[section + 1U] = std::byte{'t'};
    image[section + 2U] = std::byte{'e'};
    image[section + 3U] = std::byte{'x'};
    image[section + 4U] = std::byte{'t'};
    Put32(image, section + 8U, 0x1000U);
    Put32(image, section + 12U, 0x1000U);
    Put32(image, section + 16U, 0x200U);
    Put32(image, section + 20U, 0x200U);
    Put32(image, section + 36U, 0x60000020U);
    image[0x200U] = std::byte{0x4e};
    image[0x201U] = std::byte{0x80};
    image[0x202U] = std::byte{0x00};
    image[0x203U] = std::byte{0x20};
    return image;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 2)
    {
        std::fputs("usage: test_gears1_guest_image [normalized-image]\n", stderr);
        return 2;
    }

    std::vector<std::byte> source;
    gears::XexIdentity expected;
    if (argc == 2)
    {
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<char> raw((std::istreambuf_iterator<char>(input)), {});
        if (!input.good() && !input.eof())
        {
            std::fprintf(stderr, "FAIL could not read %s\n", argv[1]);
            return 1;
        }
        source.reserve(raw.size());
        for (const char value : raw)
        {
            source.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
        }
        constexpr std::array<std::uint8_t, 32> kGearsImageDigest{
            0xf6, 0x1c, 0xc7, 0x8e, 0x40, 0x57, 0xbc, 0x68, 0xa2, 0xc6, 0x53,
            0x86, 0xa0, 0x34, 0x1f, 0x6d, 0x26, 0xa7, 0xad, 0xd3, 0xdf, 0xd9,
            0x91, 0x80, 0x07, 0xa4, 0x55, 0x75, 0x0e, 0xc6, 0xed, 0x5c};
        expected = {.imageDigest = kGearsImageDigest,
                    .imageBase = 0x82000000U,
                    .imageSize = 13500416U,
                    .entryPoint = 0x82612bf0U};
    }
    else
    {
        source = SyntheticNormalizedImage();
        expected = {.imageDigest = x360port::HashBytes(source),
                    .imageBase = 0x82000000U,
                    .imageSize = static_cast<std::uint32_t>(source.size()),
                    .entryPoint = 0x82001000U};
    }

    gears::Gears1GuestImage module;
    std::string error;
    if (!module.Initialize(source, expected, {}, error))
    {
        std::fprintf(stderr, "FAIL valid normalized image was refused: %s\n", error.c_str());
        return 1;
    }
    const std::uint32_t expected_flat_size = argc == 2 ? 14563840U : 0x2000U;
    const std::uint32_t expected_code_base = argc == 2 ? 0x82170000U : 0x82001000U;
    if (module.Descriptor().image.size != expected_flat_size ||
        module.Descriptor().image.sha256 != x360port::HashBytes(module.ImageBytes()) ||
        module.Descriptor().code.base != expected_code_base || module.ImportManifest().size() != 0U)
    {
        std::fprintf(
            stderr,
            "FAIL flat guest module contract was not sealed: size=%u code=0x%08x imports=%zu\n",
            module.Descriptor().image.size, module.Descriptor().code.base,
            module.ImportManifest().size());
        return 1;
    }

    if (argc == 1)
    {
        auto mutated = source;
        mutated[0] ^= std::byte{0xff};
        if (module.Initialize(mutated, expected, {}, error) || error.empty())
        {
            std::fputs("FAIL profile-mismatching image was accepted\n", stderr);
            return 1;
        }
    }
    std::puts(
        "Gears normalized-image adapter: profile authentication and flat guest mapping passed");
    return 0;
}
