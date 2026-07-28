#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "gpu_draw.h"

// Capture one frame's draw stream to a self-contained file, and load it back
// without the guest.
//
// Why this exists: every question about the renderer used to cost a 200-second
// run. Reaching a gameplay frame needs a scripted controller walk from the title
// screen into Act 1, so each hypothesis was a fresh boot -- and worse, each run
// landed on a DIFFERENT game moment, so two arms of the same A/B were not
// comparable and frame numbers did not line up between them. That is not a
// measurement, it is a guess with a long wait attached.
//
// A FrameDrawInputs is just register-file snapshots, microcode blobs and a
// window of guest memory: nothing that needs the recompiled title to be running.
// gpu_draw.cpp depends on nothing but gpu_draw_xlate, so a captured frame can be
// re-rendered offline in about a second, as many times as needed, with every arm
// hitting byte-identical input.
namespace gears
{

// Everything a loaded capture owns. FrameDrawInputs holds borrowed pointers, so
// the backing storage has to outlive it -- it lives here.
struct FrameCapture
{
    FrameDrawInputs inputs;
    std::vector<uint8_t> guest;                // the reconstructed guest window
    std::vector<std::vector<uint8_t>> ucode;    // blobs the draws' pointers name
};

// Writes `in` to `path`. Guest memory is stored as its non-zero blocks, so the
// file carries the pages the draws actually read rather than 512 MiB of mostly
// nothing. Microcode is deduplicated by hash. Returns false (and says why) on
// any I/O failure -- a partial capture is deleted rather than left to be
// replayed as if it were whole.
bool WriteFrameCapture(const std::filesystem::path& path, const FrameDrawInputs& in);

// Loads a capture written by WriteFrameCapture. On success `out.inputs` is ready
// to hand to RenderFrame.
bool ReadFrameCapture(const std::filesystem::path& path, FrameCapture& out);

} // namespace gears
