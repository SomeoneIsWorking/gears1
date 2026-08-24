#include "title_profile.h"

#include <algorithm>
#include <limits>

namespace gears
{
namespace
{

[[nodiscard]] bool IsZeroDigest(const Sha256Digest &digest) noexcept
{
    return std::ranges::all_of(digest, [](std::uint8_t byte) { return byte == 0; });
}

[[nodiscard]] bool IsValidIdentity(const XexIdentity &identity) noexcept
{
    if (IsZeroDigest(identity.containerDigest) || IsZeroDigest(identity.imageDigest) ||
        identity.imageSize == 0)
    {
        return false;
    }

    const std::uint64_t imageBegin = identity.imageBase;
    const std::uint64_t imageEnd = imageBegin + identity.imageSize;
    return imageEnd <= std::numeric_limits<std::uint32_t>::max() + std::uint64_t{1} &&
           identity.entryPoint >= imageBegin && identity.entryPoint < imageEnd;
}

[[nodiscard]] bool IsPortableKey(std::string_view key) noexcept
{
    if (key.empty() || key == "." || key == ".." || key.front() == '.' || key.back() == '.')
    {
        return false;
    }

    return std::ranges::all_of(key,
                               [](char character)
                               {
                                   const auto value = static_cast<unsigned char>(character);
                                   const bool alphanumeric = (value >= 'a' && value <= 'z') ||
                                                             (value >= 'A' && value <= 'Z') ||
                                                             (value >= '0' && value <= '9');
                                   return alphanumeric || value == '_' || value == '-' ||
                                          value == '.';
                               });
}

[[nodiscard]] bool IsValidRevisionStatus(RevisionStatus status) noexcept
{
    return status == RevisionStatus::Recognized || status == RevisionStatus::Experimental ||
           status == RevisionStatus::Verified;
}

[[nodiscard]] bool IsValidCapabilityStatus(CapabilityStatus status) noexcept
{
    return status == CapabilityStatus::Unavailable || status == CapabilityStatus::Untested ||
           status == CapabilityStatus::Verified;
}

[[nodiscard]] bool IsValidProfile(const TitleProfile &profile) noexcept
{
    return IsPortableKey(profile.titleKey) && IsPortableKey(profile.revisionKey) &&
           IsPortableKey(profile.saveNamespace) && IsValidIdentity(profile.xex) &&
           IsValidRevisionStatus(profile.revisionStatus) &&
           std::ranges::all_of(profile.capabilities.status, IsValidCapabilityStatus);
}

[[nodiscard]] bool ProfilesConflict(const TitleProfile &left, const TitleProfile &right) noexcept
{
    const bool sameRevision =
        left.titleKey == right.titleKey && left.revisionKey == right.revisionKey;
    return sameRevision || left.xex == right.xex;
}

} // namespace

CapabilityStatus TitleCapabilities::Get(TitleCapability capability) const noexcept
{
    const auto index = static_cast<std::size_t>(capability);
    if (index >= status.size())
    {
        return CapabilityStatus::Unavailable;
    }
    return status[index];
}

TitleProfileResolution ResolveTitleProfile(std::span<const TitleProfile> profiles,
                                           const XexIdentity &observed) noexcept
{
    if (!IsValidIdentity(observed))
    {
        return {.error = TitleProfileError::InvalidObservedIdentity};
    }

    for (const TitleProfile &profile : profiles)
    {
        if (!IsValidProfile(profile))
        {
            return {.error = TitleProfileError::InvalidProfile};
        }
    }

    for (std::size_t left = 0; left < profiles.size(); ++left)
    {
        for (std::size_t right = left + 1; right < profiles.size(); ++right)
        {
            if (ProfilesConflict(profiles[left], profiles[right]))
            {
                return {.error = TitleProfileError::AmbiguousRegistry};
            }
        }
    }

    const auto match = std::ranges::find(profiles, observed, &TitleProfile::xex);
    if (match == profiles.end())
    {
        return {.error = TitleProfileError::UnknownBuild};
    }

    return {.profile = &*match, .error = TitleProfileError::None};
}

std::string_view TitleProfileErrorText(TitleProfileError error) noexcept
{
    switch (error)
    {
    case TitleProfileError::None:
        return "none";
    case TitleProfileError::InvalidObservedIdentity:
        return "invalid executable identity";
    case TitleProfileError::InvalidProfile:
        return "invalid generated title profile";
    case TitleProfileError::AmbiguousRegistry:
        return "ambiguous generated title registry";
    case TitleProfileError::UnknownBuild:
        return "executable does not match the generated title module";
    }
    return "unknown title profile error";
}

} // namespace gears
