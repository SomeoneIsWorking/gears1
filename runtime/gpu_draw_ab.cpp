#include "gpu_draw_ab.h"

#include <lucent/config.h>
#include <lucent/log.h>

#include "frame_probe_capture.h"

namespace gears::draw
{
namespace
{

constexpr uint64_t kWarmupFrames = 12;

} // namespace

DrawTimingAb::DrawTimingAb()
    : census_(lucent::config::flag("DRAW_AB_CENSUS")),
      untile_(lucent::config::flag("DRAW_AB_UNTILE")),
      textureDirty_(lucent::config::flag("DRAW_AB_TEXDIRTY")),
      targetLookup_(lucent::config::flag("DRAW_AB_TARGET_LOOKUP"))
{
}

void DrawTimingAb::BeginFrame()
{
    census_.BeginFrame();
    untile_.BeginFrame();
    textureDirty_.BeginFrame();
    targetLookup_.BeginFrame();

    const int armed = int(census_.Enabled()) + int(untile_.Enabled()) +
                      int(textureDirty_.Enabled()) + int(targetLookup_.Enabled());
    if (armed > 1)
        lucent::error("draw", "more than one GEARS_DRAW_AB_* timing knob is on."
                              " They alternate independently and all record the same frame"
                              " cost, so no result would mean anything. Enable ONE");
}

bool DrawTimingAb::AnyEnabled() const
{
    return census_.Enabled() || untile_.Enabled() || textureDirty_.Enabled() ||
           targetLookup_.Enabled();
}

void DrawTimingAb::CompleteFrame(double drawLoopMs, bool report, bool probe)
{
    ++renderedFrames_;
    const bool recordable =
        FrameMayRecordMeasurement(report, probe) && renderedFrames_ > kWarmupFrames;
    if (recordable)
    {
        if (census_.Enabled())
            census_.RecordFrame(drawLoopMs);
        if (untile_.Enabled())
            untile_.RecordFrame(drawLoopMs);
        if (textureDirty_.Enabled())
            textureDirty_.RecordFrame(drawLoopMs);
        if (targetLookup_.Enabled())
            targetLookup_.RecordFrame(drawLoopMs);
    }
    if (!report)
        return;

    Report(census_, "per-draw viewport census");
    Report(untile_, "EDRAM tiling collapsed");
    Report(textureDirty_, "texture staleness page-skips");
    Report(targetLookup_, "frame-local surface-target lookup");
}

void DrawTimingAb::Report(const AbTest &test, const char *what)
{
    if (!test.Enabled())
        return;
    AbSummary summary;
    if (!test.Summarise(summary))
    {
        lucent::info("draw",
                     "A/B ({}): nothing recorded yet -- every frame so"
                     " far was a report frame, which is excluded",
                     what);
        return;
    }
    if (summary.resolved)
    {
        lucent::info("draw",
                     "A/B ({}): the experimental arm is {:+.2f} ms"
                     " ({:.2f} vs {:.2f} ms over {} and {} frames), and that is"
                     " larger than the {:.2f} ms this run can resolve",
                     what, summary.differenceMs, summary.armMs, summary.baselineMs,
                     summary.armFrames, summary.baselineFrames, summary.noiseMs);
        return;
    }
    lucent::info("draw",
                 "A/B ({}): NOT RESOLVED. The arms differ by"
                 " {:+.2f} ms ({:.2f} vs {:.2f} over {} and {} frames) but this run"
                 " can only resolve {:.2f} ms, so that number is noise -- do not"
                 " read it as a small effect in either direction",
                 what, summary.differenceMs, summary.armMs, summary.baselineMs, summary.armFrames,
                 summary.baselineFrames, summary.noiseMs);
}

} // namespace gears::draw
