#include "gears1_runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
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

std::vector<std::byte> SyntheticImage()
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
    // Preserve the embedding return address around the import call.
    image[0x200U] = std::byte{0x7d};
    image[0x201U] = std::byte{0x88};
    image[0x202U] = std::byte{0x02};
    image[0x203U] = std::byte{0xa6};
    image[0x204U] = std::byte{0x48};
    image[0x205U] = std::byte{0x00};
    image[0x206U] = std::byte{0x00};
    image[0x207U] = std::byte{0x0d};
    image[0x208U] = std::byte{0x7d};
    image[0x209U] = std::byte{0x88};
    image[0x20aU] = std::byte{0x03};
    image[0x20bU] = std::byte{0xa6};
    image[0x20cU] = std::byte{0x4e};
    image[0x20dU] = std::byte{0x80};
    image[0x20eU] = std::byte{0x00};
    image[0x20fU] = std::byte{0x20};
    image[0x210U] = std::byte{0x4e};
    image[0x211U] = std::byte{0x80};
    image[0x212U] = std::byte{0x00};
    image[0x213U] = std::byte{0x20};
    return image;
}

x360port::XamPadSnapshot ReadPad(x360port::XamInputRequest, void *) noexcept
{
    return {};
}
x360port::XamPadCapabilities ReadCapabilities(x360port::XamInputRequest, void *) noexcept
{
    return {};
}

} // namespace

int main()
{
    const std::vector<std::byte> image = SyntheticImage();
    const gears::XexIdentity expected{.imageDigest = x360port::HashBytes(image),
                                      .imageBase = 0x82000000U,
                                      .imageSize = static_cast<std::uint32_t>(image.size()),
                                      .entryPoint = 0x82001000U};
    const std::array<gears::ImportSpec, 1> imports{{
        {x360port::ImportKind::Function, "xam.xex", 971U, "XGetAVPack", 0x82001010U, 0x82001010U},
    }};

    gears::Gears1Runtime runtime(7U, ReadPad, ReadCapabilities, nullptr);
    const x360port::RuntimeFailure initialized = runtime.Initialize(image, expected, imports);
    if (initialized)
    {
        std::fprintf(stderr, "runtime composition refused: %s\n", initialized.detail.c_str());
        return 1;
    }
    const x360port::ExecutionResult executed = runtime.ExecuteEntry();
    if (!executed || executed.value != 7U || runtime.Statistics() == nullptr ||
        runtime.Statistics()->translated_functions == 0U)
    {
        std::fprintf(
            stderr, "runtime composition failed: error=%u detail=%s value=%llu translations=%llu\n",
            static_cast<unsigned>(executed.failure.error), executed.failure.detail.c_str(),
            static_cast<unsigned long long>(executed.value),
            runtime.Statistics() == nullptr
                ? 0ULL
                : static_cast<unsigned long long>(runtime.Statistics()->translated_functions));
        return 1;
    }
    std::puts(
        "Gears runtime composition: authenticated image, XAM binding, and Xenia entry passed");
    return 0;
}
