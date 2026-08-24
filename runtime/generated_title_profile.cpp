#include "generated_title_profile.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

#include <ppc_config.h>

namespace gears
{
namespace
{

consteval std::uint8_t HexDigit(char character)
{
    if (character >= '0' && character <= '9')
    {
        return static_cast<std::uint8_t>(character - '0');
    }
    if (character >= 'a' && character <= 'f')
    {
        return static_cast<std::uint8_t>(character - 'a' + 10);
    }
    throw "generated SHA-256 digest is not lowercase hexadecimal";
}

template <std::size_t Size> consteval Sha256Digest ParseSha256(const char (&text)[Size])
{
    static_assert(Size == 65, "generated SHA-256 digest must contain exactly 64 digits");
    Sha256Digest digest{};
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        digest[index] = static_cast<std::uint8_t>((HexDigit(text[index * 2]) << 4) |
                                                  HexDigit(text[index * 2 + 1]));
    }
    return digest;
}

static_assert(PPC_IMAGE_BASE <= std::numeric_limits<std::uint32_t>::max());
static_assert(PPC_IMAGE_SIZE <= std::numeric_limits<std::uint32_t>::max());
static_assert(PPC_IMAGE_ENTRY_POINT <= std::numeric_limits<std::uint32_t>::max());
static_assert(PPC_IMAGE_SIZE != 0);
static_assert(PPC_IMAGE_ENTRY_POINT >= PPC_IMAGE_BASE);
static_assert(PPC_IMAGE_ENTRY_POINT < PPC_IMAGE_BASE + PPC_IMAGE_SIZE);

constexpr TitleProfile kGeneratedProfiles[] = {
    {
        .titleKey = "gears1",
        .revisionKey = PPC_IMAGE_SHA256,
        .xex =
            {
                .containerDigest = ParseSha256(PPC_XEX_SHA256),
                .imageDigest = ParseSha256(PPC_IMAGE_SHA256),
                .imageBase = static_cast<std::uint32_t>(PPC_IMAGE_BASE),
                .imageSize = static_cast<std::uint32_t>(PPC_IMAGE_SIZE),
                .entryPoint = static_cast<std::uint32_t>(PPC_IMAGE_ENTRY_POINT),
            },
        .revisionStatus = RevisionStatus::Recognized,
        .capabilities = {},
        .saveNamespace = "gears1",
    },
};

} // namespace

std::span<const TitleProfile> GeneratedTitleProfiles() noexcept
{
    return kGeneratedProfiles;
}

} // namespace gears
