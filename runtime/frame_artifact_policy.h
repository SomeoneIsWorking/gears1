#pragma once

namespace gears
{

// Expensive frame artifacts must describe the frame selected for reporting.
// Capture runs may render earlier frames to warm persistent GPU state; emitting
// those frames into the same directory creates a multi-frame corpus that a
// single-frame comparer cannot interpret.
constexpr bool ShouldCaptureFrameArtifact(bool reportFrame, bool requested)
{
    return reportFrame && requested;
}

} // namespace gears
