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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
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

    // THE MIRROR IS THE RENDERER'S, NOT THE CAPTURE'S. A capture stores the value
    // that was in effect when it was recorded, and captures outlive the value: one
    // taken while the mirror was 64 MiB replays with 606 of its 722 draws fetching
    // past it, reading zero and collapsing at clipping -- a plausible but WRONG
    // picture of a frame the live runtime renders correctly (catalog #57). So the
    // replay uses what the runtime uses today, and says so when the capture
    // disagreed. GEARS_REPLAY_MIRROR_MB still overrides, for asking what a
    // different mirror would do.
    {
        const long mb = lucent::config::number("REPLAY_MIRROR_MB", 0);
        const uint64_t want = mb > 0 ? (uint64_t(mb) << 20)
                                     : uint64_t(gears::kGuestPhysicalMirrorBytes);
        const uint32_t captured = cap.inputs.guestPhysicalMirrorBytes;
        cap.inputs.guestPhysicalMirrorBytes =
            uint32_t(std::min<uint64_t>(want, cap.inputs.guestWindowBytes));
        if (mb > 0)
            lucent::info("replay", "guest memory mirror overridden to {} MiB",
                         cap.inputs.guestPhysicalMirrorBytes >> 20);
        else if (captured != cap.inputs.guestPhysicalMirrorBytes)
            lucent::warn("replay", "this capture was recorded with a {} MiB guest"
                " memory mirror; replaying with the runtime's current {} MiB"
                " instead. Replaying the captured value would render a DIFFERENT"
                " frame from the one the runtime renders today",
                captured >> 20, cap.inputs.guestPhysicalMirrorBytes >> 20);
    }

    // GEARS_REPLAY_DUMP_SHADERS=<dir> writes every distinct microcode blob in the
    // capture, named by its hash, so a specific shader can be disassembled with
    // tools/xenos_translate --raw. Reading what a shader DOES is often the only
    // way to settle a question the registers do not answer -- e.g. how a pass
    // that samples resolved depth recombines the destination's four components.
    if (const std::string& dumpDirStr = lucent::config::text("REPLAY_DUMP_SHADERS");
        !dumpDirStr.empty())
    {
        const char* dumpDir = dumpDirStr.c_str();
        std::error_code ec;
        std::filesystem::create_directories(dumpDir, ec);
        std::set<uint64_t> written;
        uint32_t n = 0;
        for (const gears::FrameDrawItem& d : cap.inputs.draws)
        {
            const std::pair<const uint8_t*, size_t> blobs[2] = {
                {d.vsUcode, d.vsUcodeSize}, {d.psUcode, d.psUcodeSize}};
            const uint64_t hashes[2] = {d.vsHash, d.psHash};
            const char* kind[2] = {"vs", "ps"};
            for (int i = 0; i < 2; ++i)
            {
                if (!blobs[i].first || !blobs[i].second || !written.insert(hashes[i]).second)
                    continue;
                char name[64];
                std::snprintf(name, sizeof(name), "%s%016lx.bin", kind[i],
                              (unsigned long)hashes[i]);
                const std::filesystem::path out =
                    std::filesystem::path(dumpDir) / name;
                if (std::FILE* f = std::fopen(out.string().c_str(), "wb"))
                {
                    std::fwrite(blobs[i].first, 1, blobs[i].second, f);
                    std::fclose(f);
                    ++n;
                }
            }
        }
        lucent::info("replay", "dumped {} distinct shader blobs to {}", n, dumpDir);
    }

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
