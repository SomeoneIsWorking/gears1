#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include <ppc_config.h>

#include "generated_title_profile.h"
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

} // namespace

int main()
{
    const std::span<const gears::TitleProfile> profiles = gears::GeneratedTitleProfiles();
    Check(profiles.size() == 1, "one exact profile is compiled with one title module");
    if (profiles.size() == 1)
    {
        const gears::TitleProfile &profile = profiles.front();
        Check(profile.xex.imageBase == PPC_IMAGE_BASE, "profile image base comes from generator");
        Check(profile.xex.imageSize == PPC_IMAGE_SIZE, "profile image size comes from generator");
        Check(profile.xex.entryPoint == PPC_IMAGE_ENTRY_POINT,
              "profile entry point comes from generator");
        Check(profile.revisionKey == PPC_IMAGE_SHA256,
              "profile revision key is the exact normalized image digest");
        Check(profile.saveNamespace == "gears1",
              "title updates retain the stable Gears 1 save namespace");

        const gears::TitleProfileResolution exact =
            gears::ResolveTitleProfile(profiles, profile.xex);
        Check(exact && exact.profile == &profile, "generated exact identity resolves");

        gears::XexIdentity wrongContainer = profile.xex;
        wrongContainer.containerDigest.front() ^= 1;
        Check(gears::ResolveTitleProfile(profiles, wrongContainer).error ==
                  gears::TitleProfileError::UnknownBuild,
              "same-layout wrong container is refused");

        gears::XexIdentity wrongImage = profile.xex;
        wrongImage.imageDigest.front() ^= 1;
        Check(gears::ResolveTitleProfile(profiles, wrongImage).error ==
                  gears::TitleProfileError::UnknownBuild,
              "same-layout wrong normalized image is refused");
    }

    if (g_failures == 0)
    {
        std::puts("all generated title profile tests passed");
        return 0;
    }
    std::fprintf(stderr, "%d generated title profile test(s) FAILED\n", g_failures);
    return 1;
}
