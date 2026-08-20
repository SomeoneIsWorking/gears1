#pragma once

namespace gears
{

// Owns the one-frame re-arm needed when a bounded capture has already stopped
// collecting draws. A request received mid-frame deliberately waits at the
// boundary, then admits the following complete frame.
class FrameProbeCapture
{
  public:
    bool AcceptDraws(bool captureFinished) const { return !captureFinished || armed_; }

    bool ArmNextFrameAtBoundary(bool captureFinished, bool requestPending)
    {
        if (!captureFinished || armed_ || !requestPending)
            return false;
        armed_ = true;
        return true;
    }

    bool Requested(bool requestPending) const { return armed_ || requestPending; }
    void Complete() { armed_ = false; }

  private:
    bool armed_ = false;
};

constexpr bool ShouldBypassFrameSelection(bool probeRequested, bool captureFinished,
                                          bool contentSelectorHeld, bool indexSelectorHeld)
{
    return probeRequested && (captureFinished || contentSelectorHeld || indexSelectorHeld);
}

constexpr bool FrameNeedsHostPixels(bool reportRequested, bool probeRequested)
{
    return reportRequested || probeRequested;
}

constexpr bool FrameMayWriteCapture(bool diagnosticProbe)
{
    return !diagnosticProbe;
}

constexpr bool FrameMayRecordMeasurement(bool reportRequested, bool probeRequested)
{
    return !reportRequested && !probeRequested;
}

} // namespace gears
