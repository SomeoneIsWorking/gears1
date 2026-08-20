#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace gears
{

struct FrameDrawInputs;

// One authoritative renderer readback requested after launch. The normal live
// path keeps pixels on the GPU; a probe deliberately arms the existing report
// path for one frame, accepting its visible hitch instead of taxing every frame.
struct GraphicsProbeFrame
{
    uint64_t request = 0;
    uint64_t guestFrame = 0;
    bool rendered = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t draws = 0;
    uint64_t shaderPairs = 0;
    uint64_t nonBlackPixels = 0;
    uint64_t pixelHash = 0;
    double meanRed = 0.0;
    double meanGreen = 0.0;
    double meanBlue = 0.0;
    std::vector<uint8_t> rgba;
};

// Returns a monotonically increasing request id. PendingGraphicsProbeRequest
// stays non-zero until a renderer publishes a frame for that id.
uint64_t RequestGraphicsProbe();
uint64_t PendingGraphicsProbeRequest();

// Called only after RenderFrame. `pixels` is the renderer's own readback, not a
// swapchain screenshot, so it is valid headless and observes the graphics path
// before presentation can alter it.
void PublishGraphicsProbe(uint64_t request, const FrameDrawInputs &frame, bool rendered,
                          const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height);

std::shared_ptr<const GraphicsProbeFrame> LatestGraphicsProbe();
std::shared_ptr<const GraphicsProbeFrame> WaitForGraphicsProbe(uint64_t request,
                                                               std::chrono::milliseconds timeout);

} // namespace gears
