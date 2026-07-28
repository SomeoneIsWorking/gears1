// Render a captured guest frame offline, with no guest running.
//
// This is the renderer's instrument. Before it existed, every question about the
// guest-draw backend cost a 200-second scripted walk from the title screen into
// Act 1, and two runs of the same question landed on different game moments --
// different frame numbers, different draw counts, so the two arms of an A/B were
// not actually comparable. With a capture, every arm renders byte-identical
// input in about a second:
//
//   # once: reach a gameplay frame and capture it
//   GEARS_DRAW_FRAME_DUMP=scratch/frames/act1.gfr tools/capture_gameplay_frame.sh
//
//   # then, as often as needed
//   scratch/build/runtime/frame_replay scratch/frames/act1.gfr
//   GEARS_DRAW_ONLY_BASE=0x400 scratch/build/runtime/frame_replay scratch/frames/act1.gfr
//
// Every GEARS_DRAW_* knob the live backend honours works here, because this
// links the same runtime/gpu_draw.cpp. What it does NOT do is emulate: a capture
// is a recording of one frame's register state and guest memory, so it can only
// answer questions about the renderer, never about the guest that produced it.
#include <lucent/config.h>
#include <lucent/log.h>

#include <cstdlib>
#include <string>

#include "frame_capture.h"
#include "gpu_draw.h"

int main(int argc, char** argv)
{
    lucent::config::set_prefix("GEARS_");
    if (argc < 2)
    {
        lucent::error("replay", "usage: frame_replay <capture.gfr> [repeat-count]");
        lucent::error("replay", "capture one with"
            " GEARS_DRAW_FRAME_DUMP=<path> on any run that renders frames");
        return 2;
    }
    const std::string path = argv[1];
    // A repeat count exists because the first frame pays for shader translation
    // and pipeline creation; the steady-state cost is what a live run sees.
    const long repeats = argc > 2 ? std::strtol(argv[2], nullptr, 10) : 1;

    gears::FrameCapture cap;
    if (!gears::ReadFrameCapture(path, cap))
        return 1;

    bool ok = false;
    for (long i = 0; i < std::max<long>(1, repeats); ++i)
    {
        // Only the last pass reports (the census + screenshot cost ~40 ms), so a
        // repeat run measures the warm frame rather than the census.
        cap.inputs.report = (i + 1 >= std::max<long>(1, repeats));
        ok = gears::RenderFrame(cap.inputs);
        if (!ok)
        {
            lucent::error("replay", "RenderFrame failed on pass {}", i);
            return 1;
        }
    }
    return ok ? 0 : 1;
}
