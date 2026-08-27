#pragma once

// Renderer-specific interleaved timing arms.
//
// frame_ab owns the statistics. This owner binds those statistics to renderer
// controls, excludes cold/report/probe frames, and publishes one verdict form.

#include <cstdint>

#include "frame_ab.h"

namespace gears::draw
{

class DrawTimingAb
{
  public:
    DrawTimingAb();

    void BeginFrame();
    void CompleteFrame(double drawLoopMs, bool report, bool probe);

    [[nodiscard]] bool CollectCensus() const { return census_.Arm(); }
    [[nodiscard]] bool UntileEnabled() const { return untile_.Enabled(); }
    [[nodiscard]] bool UntileArm() const { return untile_.Arm(); }
    [[nodiscard]] bool TextureDirtyEnabled() const { return textureDirty_.Enabled(); }
    [[nodiscard]] bool TextureDirtyArm() const { return textureDirty_.Arm(); }
    [[nodiscard]] bool TargetLookupEnabled() const { return targetLookup_.Enabled(); }
    [[nodiscard]] bool TargetLookupArm() const { return targetLookup_.Arm(); }
    [[nodiscard]] bool AnyEnabled() const;

  private:
    static void Report(const AbTest &test, const char *what);

    AbTest census_;
    AbTest untile_;
    AbTest textureDirty_;
    AbTest targetLookup_;
    uint64_t renderedFrames_ = 0;
};

} // namespace gears::draw
