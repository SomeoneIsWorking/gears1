#include "graphics_probe.h"
#include "gpu_draw.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace
{

void Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    Check(gears::PendingGraphicsProbeRequest() == 0, "the probe starts with no pending capture");
    const uint64_t request = gears::RequestGraphicsProbe();
    Check(gears::PendingGraphicsProbeRequest() == request,
          "a request remains pending until a renderer publishes it");

    gears::FrameDrawInputs frame;
    frame.sequence = 42;
    gears::FrameDrawItem first;
    first.vsHash = 1;
    first.psHash = 2;
    gears::FrameDrawItem second;
    second.vsHash = 3;
    second.psHash = 4;
    frame.draws = {first, second};
    const std::vector<uint8_t> pixels = {
        0, 0, 0, 255, 255, 64, 32, 255,
    };
    gears::PublishGraphicsProbe(request, frame, true, pixels, 2, 1);

    const std::shared_ptr<const gears::GraphicsProbeFrame> probe =
        gears::WaitForGraphicsProbe(request, std::chrono::milliseconds(1));
    Check(bool(probe), "the published request becomes observable");
    Check(probe->guestFrame == 42 && probe->draws == 2 && probe->shaderPairs == 2,
          "probe metadata describes the captured frame");
    Check(probe->nonBlackPixels == 1, "probe statistics distinguish a lit pixel");
    Check(probe->meanRed == 127.5 && probe->meanGreen == 32.0 && probe->meanBlue == 16.0,
          "probe channel means come from the renderer bytes");
    Check(probe->rgba == pixels && probe->pixelHash != 0,
          "probe preserves and fingerprints the exact RGBA readback");
    Check(gears::PendingGraphicsProbeRequest() == 0, "publication retires the pending request");
    std::cout << "graphics probe tests passed\n";
}
