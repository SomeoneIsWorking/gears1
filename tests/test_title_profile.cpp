#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>

#include "title_profile.h"

namespace
{

int g_failures = 0;

void Check(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(message.size()), message.data());
        ++g_failures;
    }
}

[[nodiscard]] constexpr gears::Sha256Digest SyntheticDigest(std::uint8_t seed)
{
    gears::Sha256Digest digest{};
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        digest[index] = static_cast<std::uint8_t>(seed + index);
    }
    return digest;
}

[[nodiscard]] constexpr gears::XexIdentity SyntheticIdentity(std::uint8_t seed)
{
    return {
        .containerDigest = SyntheticDigest(seed),
        .imageDigest = SyntheticDigest(static_cast<std::uint8_t>(seed + 64)),
        .imageBase = 0,
        .imageSize = 64,
        .entryPoint = 8,
    };
}

[[nodiscard]] gears::TitleProfile SyntheticProfile(gears::XexIdentity identity)
{
    gears::TitleCapabilities capabilities;
    capabilities.status[static_cast<std::size_t>(gears::TitleCapability::DynarecExecution)] =
        gears::CapabilityStatus::Verified;
    capabilities.status[static_cast<std::size_t>(gears::TitleCapability::NativeRhi)] =
        gears::CapabilityStatus::Untested;

    return {
        .titleKey = "fixture_title",
        .revisionKey = "fixture_revision_a",
        .xex = identity,
        .revisionStatus = gears::RevisionStatus::Experimental,
        .capabilities = capabilities,
        .saveNamespace = "fixture-title-a",
    };
}

void CheckExactMatch()
{
    const gears::XexIdentity identity = SyntheticIdentity(1);
    const std::array profiles{SyntheticProfile(identity)};
    const gears::TitleProfileResolution resolution = gears::ResolveTitleProfile(profiles, identity);

    Check(static_cast<bool>(resolution), "exact identity resolves");
    Check(resolution.profile == &profiles.front(), "resolution refers to registry profile");
    Check(resolution.profile->revisionStatus == gears::RevisionStatus::Experimental,
          "revision status is retained");
    Check(resolution.profile->capabilities.Get(gears::TitleCapability::DynarecExecution) ==
              gears::CapabilityStatus::Verified,
          "verified capability is retained");
    Check(resolution.profile->capabilities.Get(gears::TitleCapability::NativeRhi) ==
              gears::CapabilityStatus::Untested,
          "untested capability is retained");
    Check(resolution.profile->saveNamespace == "fixture-title-a", "save namespace is retained");
}

void CheckEveryIdentityFieldIsExact()
{
    const gears::XexIdentity expected = SyntheticIdentity(2);
    const std::array profiles{SyntheticProfile(expected)};

    std::array candidates{expected, expected, expected, expected, expected};
    candidates[0].containerDigest[0] ^= 1;
    candidates[1].imageDigest[0] ^= 1;
    candidates[2].imageBase = 1;
    candidates[3].imageSize = 63;
    candidates[4].entryPoint = 9;

    for (const gears::XexIdentity &candidate : candidates)
    {
        const gears::TitleProfileResolution resolution =
            gears::ResolveTitleProfile(profiles, candidate);
        Check(!resolution && resolution.error == gears::TitleProfileError::UnknownBuild,
              "a partial identity match is refused as unknown");
    }
}

void CheckUnknownBuildIsRefused()
{
    const std::array profiles{SyntheticProfile(SyntheticIdentity(3))};
    const gears::TitleProfileResolution resolution =
        gears::ResolveTitleProfile(profiles, SyntheticIdentity(4));
    Check(!resolution && resolution.error == gears::TitleProfileError::UnknownBuild,
          "unknown build is refused");

    const std::span<const gears::TitleProfile> emptyRegistry;
    const gears::TitleProfileResolution emptyResolution =
        gears::ResolveTitleProfile(emptyRegistry, SyntheticIdentity(4));
    Check(!emptyResolution && emptyResolution.error == gears::TitleProfileError::UnknownBuild,
          "empty registry does not imply a default profile");
}

void CheckErrorDescriptions()
{
    Check(gears::TitleProfileErrorText(gears::TitleProfileError::UnknownBuild) ==
              "executable does not match a supported title revision",
          "unknown-build refusal has an actionable description");
    Check(!gears::TitleProfileErrorText(gears::TitleProfileError::InvalidProfile).empty(),
          "every registry refusal has a description");
}

void CheckInvalidObservedIdentityIsRefused()
{
    const std::array profiles{SyntheticProfile(SyntheticIdentity(5))};
    std::array invalid{SyntheticIdentity(5), SyntheticIdentity(5), SyntheticIdentity(5),
                       SyntheticIdentity(5), SyntheticIdentity(5)};
    invalid[0].containerDigest = {};
    invalid[1].imageDigest = {};
    invalid[2].imageSize = 0;
    invalid[3].entryPoint = invalid[3].imageSize;
    invalid[4].imageBase = std::numeric_limits<std::uint32_t>::max();
    invalid[4].imageSize = 2;
    invalid[4].entryPoint = invalid[4].imageBase;

    for (const gears::XexIdentity &identity : invalid)
    {
        const gears::TitleProfileResolution resolution =
            gears::ResolveTitleProfile(profiles, identity);
        Check(!resolution && resolution.error == gears::TitleProfileError::InvalidObservedIdentity,
              "malformed observed identity is refused before matching");
    }
}

void CheckInvalidProfileIsRefused()
{
    const gears::XexIdentity identity = SyntheticIdentity(6);

    gears::TitleProfile invalidNamespace = SyntheticProfile(identity);
    invalidNamespace.saveNamespace = "../escape";
    Check(gears::ResolveTitleProfile(std::span{&invalidNamespace, 1}, identity).error ==
              gears::TitleProfileError::InvalidProfile,
          "unsafe save namespace invalidates the registry");

    gears::TitleProfile invalidRevision = SyntheticProfile(identity);
    invalidRevision.revisionKey = "revision/path";
    Check(gears::ResolveTitleProfile(std::span{&invalidRevision, 1}, identity).error ==
              gears::TitleProfileError::InvalidProfile,
          "unsafe revision key invalidates the registry");
}

void CheckRegistryAmbiguityIsRefused()
{
    const gears::XexIdentity identity = SyntheticIdentity(7);
    std::array duplicateIdentity{SyntheticProfile(identity), SyntheticProfile(identity)};
    duplicateIdentity[1].titleKey = "fixture_title_b";
    duplicateIdentity[1].revisionKey = "fixture_revision_b";
    Check(gears::ResolveTitleProfile(duplicateIdentity, identity).error ==
              gears::TitleProfileError::AmbiguousRegistry,
          "duplicate exact identity invalidates the registry");

    std::array duplicateRevision{SyntheticProfile(identity),
                                 SyntheticProfile(SyntheticIdentity(8))};
    Check(gears::ResolveTitleProfile(duplicateRevision, identity).error ==
              gears::TitleProfileError::AmbiguousRegistry,
          "one revision key cannot name multiple exact identities");
}

} // namespace

int main()
{
    CheckExactMatch();
    CheckEveryIdentityFieldIsExact();
    CheckUnknownBuildIsRefused();
    CheckErrorDescriptions();
    CheckInvalidObservedIdentityIsRefused();
    CheckInvalidProfileIsRefused();
    CheckRegistryAmbiguityIsRefused();

    if (g_failures == 0)
    {
        std::puts("all title profile tests passed");
        return 0;
    }

    std::fprintf(stderr, "%d title profile test(s) FAILED\n", g_failures);
    return 1;
}
