#include <cstdio>

#include "frame_probe_capture.h"

namespace
{

void Check(bool actual, bool expected, const char *name, int &failures)
{
    if (actual == expected)
        return;
    std::printf("FAIL %s: got %d, expected %d\n", name, actual, expected);
    ++failures;
}

} // namespace

int main()
{
    int failures = 0;
    gears::FrameProbeCapture capture;

    Check(capture.AcceptDraws(false), true, "normal capture accepts draws", failures);
    Check(capture.ArmNextFrameAtBoundary(false, true), false, "an active capture needs no re-arm",
          failures);
    Check(capture.AcceptDraws(true), false, "a completed capture rejects draws", failures);
    Check(capture.ArmNextFrameAtBoundary(true, true), true,
          "a pending probe arms at the completed capture boundary", failures);
    Check(capture.AcceptDraws(true), true, "the armed probe accepts the next frame", failures);
    Check(capture.Requested(false), true, "the arm retains the request across the frame", failures);
    capture.Complete();
    Check(capture.AcceptDraws(true), false, "publication retires the re-arm", failures);

    Check(gears::ShouldBypassFrameSelection(true, false, true, false), true,
          "a probe bypasses a held content selector", failures);
    Check(gears::ShouldBypassFrameSelection(true, false, false, true), true,
          "a probe bypasses a held index selector", failures);
    Check(gears::ShouldBypassFrameSelection(true, true, false, false), true,
          "a probe bypasses a completed bounded capture", failures);
    Check(gears::ShouldBypassFrameSelection(false, true, true, true), false,
          "capture state alone never bypasses selection", failures);
    Check(gears::ShouldBypassFrameSelection(true, false, false, false), false,
          "a selected frame remains a normal capture", failures);

    Check(gears::FrameNeedsHostPixels(false, true), true,
          "a probe requests host pixels without a report", failures);
    Check(gears::FrameNeedsHostPixels(true, false), true, "a report requests host pixels",
          failures);
    Check(gears::FrameNeedsHostPixels(false, false), false,
          "an ordinary shared-device frame skips readback", failures);
    Check(gears::FrameMayWriteCapture(true), false,
          "a diagnostic frame cannot write capture artifacts", failures);
    Check(gears::FrameMayWriteCapture(false), true, "a selected frame may write capture artifacts",
          failures);
    Check(gears::FrameMayRecordMeasurement(false, true), false,
          "a probe cannot enter performance measurements", failures);
    Check(gears::FrameMayRecordMeasurement(false, false), true,
          "an ordinary warm frame may enter performance measurements", failures);

    if (failures == 0)
        std::puts("frame probe capture tests passed");
    return failures != 0;
}
