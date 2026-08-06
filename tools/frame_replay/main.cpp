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
#include "frame_content.h"
#include "gpu_draw.h"
#include "gpu_draw_xlate.h"
#include "render_ab.h"

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

    // GEARS_SKINNED_CHECK=1 answers ONE question about a capture and renders
    // nothing: does this frame contain a skinned character? That is the question
    // that decided whether three separate four-minute capture attempts had
    // produced anything usable (catalog #77), and it was answered by hand each
    // time. Exit 0 when the frame contains one, 3 when it does not, so a shell
    // loop can select captures without a person reading the log.
    //
    // The same scan is what the runtime's GEARS_DRAW_FRAME_DUMP_SKINNED gate
    // uses, deliberately: this mode is how that gate is validated against
    // captures whose answer is already known, rather than trusted because it
    // looks reasonable.
    if (lucent::config::flag("SKINNED_CHECK"))
    {
        const long minIdx = lucent::config::number("SKINNED_MIN_INDICES",
                                                   gears::kDefaultSkinnedMinIndices);
        const gears::SkinnedFrameCensus c =
            gears::ScanForSkinnedCharacter(cap.inputs, uint32_t(minIdx));
        gears::ReportSkinnedFrameCensus(c);
        if (lucent::config::flag("SKINNED_CHECK_LIST"))
        {
            // Every skinned draw, whatever its size. This is what the threshold
            // is CHOSEN from -- a threshold picked without looking at both
            // classes' distributions is a guess with a number in it.
            uint32_t shown = 0;
            for (uint32_t i = 0; i < cap.inputs.draws.size(); ++i)
            {
                const gears::FrameDrawItem& d = cap.inputs.draws[i];
                if (!d.vsUcode || !d.vsUcodeSize)
                    continue;
                const gears::draw::VertexShaderShape s =
                    gears::draw::AnalyzeVertexShaderShape(d.vsUcode, d.vsUcodeSize,
                                                          d.vsHash);
                if (!s.ok || !s.floatDynamicAddressing)
                    continue;
                ++shown;
                lucent::info("replay", "  skinned draw {}: {} indices, prim {},"
                    " vs {:#018x}, {} float constants", i, d.indexCount,
                    d.primType, d.vsHash, s.floatCount);
            }
            if (shown == 0)
                lucent::info("replay", "  (no skinned draw at ANY size in this"
                    " frame's {} draws)", cap.inputs.draws.size());
        }
        if (!c.available)
            return 2;
        return c.Passed() ? 0 : 3;
    }

    // ---- the interleaved render comparer --------------------------------
    //
    // GEARS_DRAW_AB=<KNOB> renders the frame TWICE IN THIS PROCESS -- once with
    // the knob unset, once with it set -- capturing a per-draw signature of the
    // surface each time, and reports the FIRST draw at which the two diverge.
    //
    // Two separate runs plus a diff cannot do this honestly. The arms have to
    // issue the SAME draws for "the first divergent draw" to mean anything, and
    // a knob that changes the draw stream silently invalidates every row after
    // it; in one process the prepared list is rebuilt identically and the
    // comparison is checked rather than assumed. It also removes the operator
    // from the loop: the answer is a draw index and a shader hash, not a pair of
    // files to eyeball.
    if (const std::string& arm = lucent::config::text("DRAW_AB"); !arm.empty())
    {
        // Under the run's own output directory, never /tmp: this machine's tmpfs
        // is a per-user quota that logs and dumps exhaust, and a comparer that
        // fills it breaks every other run on the box (catalog #80).
        const std::string& dirStr = lucent::config::text("DRAW_DIR");
        const std::filesystem::path dir =
            (dirStr.empty() ? std::filesystem::path("scratch/screenshots")
                            : std::filesystem::path(dirStr)) / "ab";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string live = (dir / "arm_live.tsv").string();
        const std::string aPath = (dir / "arm_a.tsv").string();
        const std::string bPath = (dir / "arm_b.tsv").string();
        const std::string knobEnv = "GEARS_" + arm;
        ::setenv("GEARS_DRAW_TRACE_ALL", live.c_str(), 1);

        auto renderInto = [&](const std::string& keep, bool setKnob) {
            if (setKnob) ::setenv(knobEnv.c_str(), "1", 1);
            else         ::unsetenv(knobEnv.c_str());
            // STOPGAP: add lucent::config::refresh() upstream, because lucent
            // caches every config read and the only public way to drop those
            // caches is a prefix change, which early-returns when the prefix is
            // unchanged -- so this bounces the prefix to a dummy and back.
            // Without it the second arm reads the FIRST arm's cached answers,
            // both arms render identically, and the comparer reports "no
            // divergence" for a knob that does plenty. The risk of the bounce is
            // that it also re-arms the debug channels, so a channel enabled
            // mid-run would be re-read here; nothing does that today.
            lucent::config::set_prefix("GEARS_AB_BOUNCE_");
            lucent::config::set_prefix("GEARS_");
            // EACH ARM IS A CLEAN RENDER. Almost every knob worth comparing is
            // consumed while BUILDING persistent state -- a surface's host
            // format is chosen once, when the surface is created -- so without
            // this, arm B inherits arm A's surfaces and pipelines and the knob
            // appears to change nothing, which is indistinguishable from a knob
            // that genuinely changes nothing.
            gears::ResetRendererForComparison();
            cap.inputs.report = false;
            if (!gears::RenderFrame(cap.inputs))
                return false;
            // The probe's output path is read once, so both arms write the same
            // file; snapshot it rather than trying to re-point it mid-process.
            std::error_code e;
            std::filesystem::copy_file(live, keep,
                std::filesystem::copy_options::overwrite_existing, e);
            if (e)
            {
                lucent::error("replay", "A/B: cannot keep {} as {}: {}", live,
                              keep, e.message());
                return false;
            }
            return true;
        };
        lucent::info("replay", "A/B: arm A is {} UNSET, arm B is {}=1."
            " Rendering both in this process", knobEnv, knobEnv);
        if (!renderInto(aPath, false) || !renderInto(bPath, true))
        {
            lucent::error("replay", "A/B: a render failed, so NOTHING is"
                " compared");
            return 1;
        }
        ::unsetenv(knobEnv.c_str());
        ::unsetenv("GEARS_DRAW_TRACE_ALL");
        return gears::ReportAbDivergence(aPath, bPath, knobEnv) ? 0 : 1;
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
