#include <cstdio>

#include "frame_artifact_policy.h"

int main()
{
    int failures = 0;
    const auto check = [&](bool actual, bool expected, const char* name) {
        if (actual == expected)
            return;
        std::printf("FAIL %s: got %d, expected %d\n", name, actual, expected);
        ++failures;
    };

    check(gears::ShouldCaptureFrameArtifact(true, true), true,
          "requested artifact on the reported frame");
    check(gears::ShouldCaptureFrameArtifact(false, true), false,
          "requested artifact on a warm-up frame");
    check(gears::ShouldCaptureFrameArtifact(true, false), false,
          "unrequested artifact on the reported frame");
    check(gears::ShouldCaptureFrameArtifact(false, false), false,
          "unrequested artifact on a warm-up frame");

    if (failures == 0)
        std::puts("frame artifact policy tests passed");
    return failures != 0;
}
