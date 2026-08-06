// The frame's mid-render probes. gpu_draw_probe.h says what each one answers;
// this is what each records and what each prints.

#include "gpu_draw_probe.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <string>

#include <lucent/config.h>
#include <lucent/log.h>

#include "gpu_draw_formats.h"
#include "gpu_draw_pixels.h"

namespace gears::draw
{

void FrameProbe::Build(size_t nDraws, VkDeviceSize readbackBytes)
{
    rbBytes = readbackBytes;
    // Checkpoint dumps (GEARS_DRAW_FRAME_STEP=N): after every N draws the colour
    // target is copied to its own readback buffer and written out, so the frame
    // can be attributed to individual draws instead of guessed at.
    // Which surface the probes are allowed to sample. Parsed as hex, since every
    // other place an EDRAM base appears in this runtime is hex.
    {
        const std::string& t = lucent::config::text("DRAW_SURFACE");
        if (!t.empty())
        {
            onlySurface = int64_t(std::strtoul(t.c_str(), nullptr, 16));
            lucent::info("draw", "probes restricted to EDRAM surface {:#x}:"
                " samples on any other surface are DROPPED, so a row here means"
                " that draw changed the pixel on THIS surface", onlySurface);
        }
    }
    stepEvery = lucent::config::number("DRAW_FRAME_STEP", 0);
    stepFrom = lucent::config::number("DRAW_FRAME_STEP_FROM", 0);
    if (stepEvery > 0)
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {W, H, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        uint32_t type = 0;
        VkMemoryRequirements req{};
        if (vkCreateImage(R.device, &ci, nullptr, &checkpointStage) == VK_SUCCESS)
        {
            vkGetImageMemoryRequirements(R.device, checkpointStage, &req);
            if (!R.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
                R.FindMemory(req.memoryTypeBits, 0, type);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type;
            if (vkAllocateMemory(R.device, &ai, nullptr, &checkpointStageMem) != VK_SUCCESS ||
                vkBindImageMemory(R.device, checkpointStage, checkpointStageMem, 0) != VK_SUCCESS)
            {
                vkDestroyImage(R.device, checkpointStage, nullptr);
                checkpointStage = VK_NULL_HANDLE;
                lucent::warn("draw", "GEARS_DRAW_FRAME_STEP: no staging image, so"
                    " checkpoints of non-8888 surfaces will be skipped");
            }
        }
    }

    // GEARS_DRAW_PIXEL_TRACE=<x>,<y>: WHICH DRAW PAINTED THIS PIXEL.
    //
    // Attributing a visible defect to a draw is the question this renderer asks
    // most, and until now the only answer was GEARS_DRAW_FRAME_STEP -- whole
    // images, capped at 48, and read back through an 8-bit blit that clamps. On a
    // base pass whose HDR target is mostly above 1.0 that blit reports 255 for
    // every pixel of interest, so the instrument cannot separate two draws at all.
    // This copies ONE texel after every draw, uncapped, in the surface's own
    // format, and prints the draws where it CHANGED -- with the denominator, so a
    // trace that never changes is distinguishable from a trace that never ran.
    if (const std::string& spec = lucent::config::text("DRAW_PIXEL_TRACE"); !spec.empty())
    {
        const size_t comma = spec.find(',');
        if (comma == std::string::npos)
            lucent::warn("draw", "GEARS_DRAW_PIXEL_TRACE: cannot parse '{}',"
                " expected <x>,<y>; NOT tracing", spec);
        else
        {
            traceX = std::atoi(spec.c_str());
            traceY = std::atoi(spec.c_str() + comma + 1);
            if (traceX < 0 || traceY < 0 || uint32_t(traceX) >= W || uint32_t(traceY) >= H)
            {
                lucent::warn("draw", "GEARS_DRAW_PIXEL_TRACE: ({},{}) is outside the"
                    " {}x{} surface; NOT tracing", traceX, traceY, W, H);
                traceX = traceY = -1;
            }
            // 16 bytes per sample covers the widest surface format here.
            else if (!R.MakeBuffer(16ull * (nDraws + 2),
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, pixelBuf, pixelMem, true))
            {
                lucent::warn("draw", "GEARS_DRAW_PIXEL_TRACE: no readback buffer;"
                    " NOT tracing");
                traceX = traceY = -1;
            }
        }
    }
}

bool FrameProbe::CheckpointDue(uint32_t drawn) const
{
    // Highest issued-draw index this frame reached, so Report can explain a
    // window that the frame never got to.
    if (drawn > highestDrawn)
        highestDrawn = drawn;
    // GEARS_DRAW_FRAME_STEP_FROM=N aims the (capped) window of checkpoints at
    // the draws that matter. Without it the cap always lands on the FIRST 48
    // steps, so a defect introduced late in a frame -- the UI, the post chain --
    // can never be attributed no matter what step size is chosen.
    return stepEvery > 0 && drawn > 0 && long(drawn) >= stepFrom &&
           (drawn % uint32_t(stepEvery)) == 0;
}

// A checkpoint dumps the surface that was being rendered into at that
// point, and only when that surface is 8888 -- the readback path is 8-bit
// RGBA, and an HDR surface's bytes are not pixels.
// WHY THIS BLITS RATHER THAN REFUSING. The old version returned unless the
// surface was already 8888, and said nothing when it did. Every surface in
// this title's frames is R16G16B16A16_SFLOAT, so GEARS_DRAW_FRAME_STEP was a
// SILENT NO-OP: it wrote no images, logged no lines, and looked exactly like a
// frame with nothing to report. An HDR surface's bytes are indeed not pixels --
// so convert them, the same way the presented frame already is.
void FrameProbe::Checkpoint(VkCommandBuffer cmd, uint32_t drawsSoFar,
                            const SurfaceTarget* t, uint32_t surfaceBase)
{
    // Restricted to one surface when asked: a checkpoint of whichever target
    // happened to be bound cannot answer "which draw changed this pixel".
    if (!SurfaceWanted(surfaceBase))
        return;
    // SAY SO. A checkpoint that produces nothing must not look like a
    // checkpoint that found nothing -- the run's log is the only place the
    // difference is visible.
    if (!t || !t->begunThisFrame)
    {
        lucent::info("draw", "  checkpoint after {} draws: NOT TAKEN -- {}",
            drawsSoFar, t ? "the surface has not been rendered to this frame"
                          : "no surface has been opened yet");
        return;
    }
    if (checkpoints.size() >= kMaxCheckpoints)
    {
        ++checkpointsSkipped;
        return;
    }
    VkBuffer b = 0; VkDeviceMemory m = 0;
    if (!R.MakeBuffer(rbBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b, m))
        return;
    VkImage source = t->color;
    if (t->hostFormat != VK_FORMAT_R8G8B8A8_UNORM && checkpointStage != VK_NULL_HANDLE)
    {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = checkpointStage; bar.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {int32_t(W), int32_t(H), 1};
        bl.dstOffsets[1] = {int32_t(W), int32_t(H), 1};
        vkCmdBlitImage(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            checkpointStage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
            VK_FILTER_NEAREST);
        VkImageMemoryBarrier rb = bar;
        rb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        rb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        rb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        rb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &rb);
        source = checkpointStage;
    }
    VkBufferImageCopy rg{};
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        b, 1, &rg);
    checkpoints.push_back(Checkpt{drawsSoFar, b, surfaceBase});
    checkpointMem.push_back(m);
}

void FrameProbe::TracePixel(VkCommandBuffer cmd, uint32_t drawsSoFar,
                            const SurfaceTarget* t, uint32_t surfaceBase)
{
    if (traceX < 0 || !t || !t->begunThisFrame)
        return;
    if (!SurfaceWanted(surfaceBase))
        return;
    VkBufferImageCopy rg{};
    rg.bufferOffset = VkDeviceSize(pixelSamples.size()) * 16;
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageOffset = {traceX, traceY, 0};
    rg.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        pixelBuf, 1, &rg);
    pixelSamples.push_back(PixelSample{drawsSoFar, surfaceBase, t->hostFormat});
}

void FrameProbe::Report(const std::vector<PreparedDraw>& prepared)
{
    // Checkpoint images, each labelled with how many draws had run.
    const std::string& checkpointDirStr = lucent::config::text("DRAW_DIR");
    const std::filesystem::path outDir = checkpointDirStr.empty()
        ? std::filesystem::path("scratch/screenshots")
        : std::filesystem::path(checkpointDirStr);
    std::vector<uint8_t> cp(rbBytes);
    for (size_t i = 0; i < checkpoints.size(); ++i)
    {
        void* p = nullptr;
        if (vkMapMemory(R.device, checkpointMem[i], 0, rbBytes, 0, &p) != VK_SUCCESS)
            continue;
        std::memcpy(cp.data(), p, rbBytes);
        vkUnmapMemory(R.device, checkpointMem[i]);
        uint64_t cpLit = 0, cpNonBlack = 0;
        for (uint32_t k = 0; k < W * H; ++k)
        {
            const uint8_t* px = &cp[size_t(k) * 4];
            if (!(px[0] == 13 && px[1] == 13 && px[2] == 20))
                ++cpLit;
            if (px[0] || px[1] || px[2])
                ++cpNonBlack;
        }
        const std::string name = std::format("frame_after{:04}.ppm", checkpoints[i].draws);
        WritePpm(outDir / name, cp.data(), W, H);
        lucent::info("draw", "  checkpoint after {} draws on surface {:#x}:"
            " {} px != clear, {} px non-black -> {}", checkpoints[i].draws,
            checkpoints[i].surface, cpLit, cpNonBlack, name);
    }
    // The pixel trace. Printed as CHANGES, because 650 identical rows hide the
    // handful that matter -- but with the denominator and the final value, so
    // "nothing changed it" cannot be confused with "nothing was traced".
    if (traceX >= 0)
    {
        std::vector<uint8_t> raw(16ull * (pixelSamples.size() + 1), 0);
        void* p = nullptr;
        if (!pixelSamples.empty() &&
            vkMapMemory(R.device, pixelMem, 0, raw.size(), 0, &p) == VK_SUCCESS)
        {
            std::memcpy(raw.data(), p, raw.size());
            vkUnmapMemory(R.device, pixelMem);
        }
        auto decode = [&](size_t i) {
            std::array<float, 4> v{0, 0, 0, 0};
            const uint8_t* b = raw.data() + i * 16;
            if (pixelSamples[i].format == VK_FORMAT_R8G8B8A8_UNORM)
                for (int k = 0; k < 4; ++k) v[k] = float(b[k]) / 255.0f;
            else  // R16G16B16A16_SFLOAT, this title's every surface
                for (int k = 0; k < 4; ++k)
                {
                    uint16_t h; std::memcpy(&h, b + k * 2, 2);
                    v[k] = HalfToFloat(h);
                }
            return v;
        };
        lucent::Line tl;
        tl.add("pixel trace ({},{}): {} sample(s), one after every draw. Rows are"
               " the draws that CHANGED it:", traceX, traceY, pixelSamples.size());
        uint32_t changes = 0;
        std::array<float, 4> prev{-1, -1, -1, -1};
        for (size_t i = 0; i < pixelSamples.size(); ++i)
        {
            const std::array<float, 4> v = decode(i);
            if (i != 0 && v == prev)
                continue;
            prev = v;
            ++changes;
            const uint32_t n = pixelSamples[i].draws;
            // The draw that produced this value is the one issued just before the
            // sample, i.e. prepared[n-1]; naming prepared[n] would blame the next.
            const PreparedDraw* by = (n >= 1 && n <= prepared.size())
                                   ? &prepared[n - 1] : nullptr;
            tl.add("\n  after {} draws (surface {:#x}) = ({}, {}, {}, {}){}",
                   n, pixelSamples[i].surface, v[0], v[1], v[2], v[3],
                   by ? std::format(" <- draw {} ps {:#x}", by->diagIndex, by->psHash)
                      : std::string(" <- (before any draw)"));
        }
        if (changes <= 1)
            tl.add("\n  NOTHING after the first sample changed it. That is a real"
                   " negative only if the trace ran: {} samples were taken.",
                   pixelSamples.size());
        tl.flush(lucent::Level::Info, "draw");
    }
    // NO SILENT TRUNCATION. A capped census that does not say it was capped reads
    // as full coverage of the frame.
    if (checkpointsSkipped != 0)
        lucent::warn("draw", "GEARS_DRAW_FRAME_STEP: {} further checkpoints were"
            " DROPPED at the {}-checkpoint cap -- the images above stop partway"
            " through the frame, they do not cover it", checkpointsSkipped,
            kMaxCheckpoints);
    // A WINDOW THE FRAME NEVER REACHED PRODUCED ABSOLUTE SILENCE, and silence
    // here reads as "the checkpoints found nothing interesting" rather than
    // "no checkpoint was ever taken".
    //
    // The trap is a units mismatch, and it is easy to walk into: _FROM counts
    // the draws this renderer ISSUED, while the number you naturally reach for
    // is the `draw` column of the diag table or a pass_structure listing, which
    // are GUEST draw indices. They differ by every draw the frame drops -- with
    // the tiling collapse on, act1 issues 527 of 737, so every guest index above
    // 527 names a checkpoint that can never fire.
    if (stepEvery > 0 && checkpoints.empty())
        lucent::warn("draw", "GEARS_DRAW_FRAME_STEP: NO checkpoint was taken."
            " _FROM={} but this frame only issued {} draws, and _FROM counts"
            " ISSUED draws -- not the `draw` column of the diag table, which is"
            " the guest's index and is larger whenever draws are dropped or"
            " collapsed. Nothing below describes the frame", stepFrom,
            highestDrawn);
}

void FrameProbe::Release()
{
    if (pixelBuf != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(R.device, pixelBuf, nullptr);
        vkFreeMemory(R.device, pixelMem, nullptr);
        pixelBuf = VK_NULL_HANDLE;
    }
    if (checkpointStage != VK_NULL_HANDLE)
    {
        vkDestroyImage(R.device, checkpointStage, nullptr);
        vkFreeMemory(R.device, checkpointStageMem, nullptr);
        checkpointStage = VK_NULL_HANDLE;
    }
    for (size_t i = 0; i < checkpoints.size(); ++i)
    {
        vkDestroyBuffer(R.device, checkpoints[i].buffer, nullptr);
        vkFreeMemory(R.device, checkpointMem[i], nullptr);
    }
    checkpoints.clear();
    checkpointMem.clear();
}


void DrawStats::Build(VkCommandBuffer cmd, size_t nDraws)
{
    const std::string& dp = lucent::config::text("DRAW_DIAG");
    diagPath = dp;
    const bool enabled = (lucent::config::flag("DRAW_STATS") || !diagPath.empty()) &&
                         R.hasPipelineStats &&
                         lucent::config::number("DRAW_ONLY", -1) < 0;
    if (!enabled)
        return;
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    qpi.queryCount = uint32_t(nDraws);
    qpi.pipelineStatistics =
        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
        VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    if (vkCreateQueryPool(R.device, &qpi, nullptr, &pool) != VK_SUCCESS)
        pool = VK_NULL_HANDLE;
    else
        vkCmdResetQueryPool(cmd, pool, 0, uint32_t(nDraws));
}

void DrawStats::Begin(VkCommandBuffer cmd, uint32_t drawIndex)
{
    if (pool != VK_NULL_HANDLE)
        vkCmdBeginQuery(cmd, pool, drawIndex, 0);
}

void DrawStats::End(VkCommandBuffer cmd, uint32_t drawIndex)
{
    if (pool != VK_NULL_HANDLE)
        vkCmdEndQuery(cmd, pool, drawIndex);
}

void DrawStats::Report(uint32_t drawn, const std::vector<PreparedDraw>& prepared)
{
    if (pool == VK_NULL_HANDLE)
        return;
    std::vector<uint64_t> st(size_t(drawn) * kCounters, 0);
    if (drawn > 0 &&
        vkGetQueryPoolResults(R.device, pool, 0, drawn,
            st.size() * sizeof(uint64_t), st.data(),
            kCounters * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
    {
        uint32_t noPrims = 0, noFrags = 0, shaded = 0;
        for (uint32_t i = 0; i < drawn; ++i)
        {
            const uint64_t* s = &st[size_t(i) * kCounters];
            if (s[2] == 0) ++noPrims;
            else if (s[3] == 0) ++noFrags;
            else ++shaded;
            lucent::debug("draw", "  stats draw {}: {} verts, {} prims in,"
                " {} prims after clip+cull, {} fragment invocations",
                i, s[0], s[1], s[2], s[3]);
        }
        lucent::info("draw", "frame pipeline statistics: {} draws produced no"
            " primitive after clip+cull, {} produced primitives but no fragment,"
            " {} ran the fragment shader", noPrims, noFrags, shaded);
        if (!diagPath.empty())
            WriteTable(drawn, prepared, st);
    }
    vkDestroyQueryPool(R.device, pool, nullptr);
    pool = VK_NULL_HANDLE;
}

void DrawStats::WriteTable(uint32_t drawn, const std::vector<PreparedDraw>& prepared,
                           const std::vector<uint64_t>& st)
{
    std::error_code ec;
    const std::filesystem::path dp(diagPath);
    if (dp.has_parent_path())
        std::filesystem::create_directories(dp.parent_path(), ec);
    std::ofstream t(dp, std::ios::binary);
    if (!t)
    {
        lucent::error("draw", "per-draw diagnostic: cannot write {}", diagPath);
    }
    else
    {
        t << "draw\tsurface\tcolor_fmt\tcolor_exp_bias\tedram_mode\tprim\tprim_name"
             "\tindexed\tcount\tfrag_stage\tia_verts\tia_prims"
             "\tprims_after_clip\tfrag_invocations\tverdict"
             "\tdepth_test\tdepth_write\tdepth_func\tcolor_mask"
             "\tblend_on\tblend0\tvp_x\tvp_y\tvp_w\tvp_h\tvp_minz"
             "\tvp_maxz\tsc_x\tsc_y\tsc_w\tsc_h"
             "\tclip_cntl\tsu_sc_mode\tvte_cntl\twindow_offset"
             "\tvport_xs\tvport_xo\tvport_ys\tvport_yo"
             "\tvport_zs\tvport_zo\tvs_hash\tps_hash"
             "\tresolve_dest\tresolve_src\tresolve_dst"
             "\tresolve_is_depth\tresolve_clears_depth"
             // What the guest asked this resolve to DO to the colour on its
             // way out of EDRAM. RB_COPY_DEST_INFO's red/blue swap decides
             // whether the destination holds the surface's channel order or
             // the reverse, and it is per-resolve: one frame sets it on one
             // destination and not another. Without it in the table, "which
             // buffer is swapped relative to which" is a question you can
             // only answer by toggling a knob and diffing images, which is
             // how catalog #62 stayed open.
             "\tresolve_swap_rb\tresolve_scale\n";
        uint32_t row = 0;
        for (const PreparedDraw& pd : prepared)
        {
            // RESOLVES ARE ROWS TOO. They used to be skipped, and
            // that made the frame's PASS STRUCTURE invisible: a
            // resolve is exactly where one UE3 pass ends and the
            // next begins (BeginRenderingSceneColor /
            // FinishRenderingSceneColor / ResolveSceneDepthTexture
            // in SceneRendering.cpp), so a table of only draws
            // shows a flat stream with no seams in it. The columns
            // a resolve has nothing to say about are left empty
            // rather than zeroed, so "0 primitives" and "not a
            // draw" cannot be confused by whatever reads this.
            if (pd.isResolve)
            {
                t << pd.diagIndex << '\t' << std::hex << "0x"
                  << pd.surfaceBase << std::dec
                  << '\t' << pd.colorFormat << '\t' << pd.colorExpBias
                  << '\t' << pd.edramMode
                  << "\t\tresolve\t\t\t\t\t\t\t\tresolve"
                  // depth/colour-mask/blend/viewport columns: a
                  // resolve programs none of them.
                  << "\t\t\t\t\t\t"
                  << "\t\t\t\t\t\t"
                  << '\t' << pd.resolveSrcRect.offset.x
                  << '\t' << pd.resolveSrcRect.offset.y
                  << '\t' << pd.resolveSrcRect.extent.width
                  << '\t' << pd.resolveSrcRect.extent.height
                  << "\t\t\t\t"          // clip/su_sc/vte/window
                  << "\t\t\t\t\t\t"      // vport scale/offset
                  << "\t\t"              // vs_hash / ps_hash
                  << '\t' << std::hex << "0x" << pd.resolveDest
                  << std::dec
                  << '\t' << pd.resolveSrcRect.extent.width << 'x'
                  << pd.resolveSrcRect.extent.height
                  << '\t' << pd.resolveDstX << ',' << pd.resolveDstY
                  << '\t' << (pd.resolveIsDepth ? 1 : 0)
                  << '\t' << (pd.clearsDepth ? 1 : 0)
                  << '\t' << (pd.resolveSwapRB ? 1 : 0)
                  << '\t' << pd.resolveScale
                  << '\n';
                continue;
            }
            if (row >= drawn)
            { ++row; continue; }
            const uint64_t* s = &st[size_t(row) * kCounters];
            // The verdict is the whole point: it says which stage
            // this draw died at, in the vocabulary the pipeline
            // statistics can actually support.
            const char* verdict =
                s[1] == 0 ? "no_primitive_assembled" :
                s[2] == 0 ? "killed_by_clip_or_cull" :
                s[3] == 0 ? "rasterised_no_fragment" :
                !pd.hasFragmentStage ? "depth_only_no_colour" :
                pd.colorMask == 0 ? "colour_fully_masked" : "shaded";
            t << pd.diagIndex << '\t' << std::hex << "0x" << pd.surfaceBase
              << std::dec << '\t' << pd.colorFormat
              << '\t' << pd.colorExpBias << '\t' << pd.edramMode
              << '\t' << pd.primType << '\t' << PrimName(pd.primType)
              << '\t' << (pd.indexed ? 1 : 0) << '\t' << pd.count
              << '\t' << (pd.hasFragmentStage ? 1 : 0)
              << '\t' << s[0] << '\t' << s[1] << '\t' << s[2] << '\t' << s[3]
              << '\t' << verdict
              << '\t' << ((pd.depthControl >> 1) & 1)
              << '\t' << ((pd.depthControl >> 2) & 1)
              << '\t' << ((pd.depthControl >> 4) & 7)
              << '\t' << (pd.colorMask & 0xF)
              << '\t' << (BlendIsIdentity(pd.blend0) ? 0 : 1)
              << '\t' << std::hex << "0x" << pd.blend0 << std::dec
              << '\t' << pd.viewport.x << '\t' << pd.viewport.y
              << '\t' << pd.viewport.width << '\t' << pd.viewport.height
              << '\t' << pd.viewport.minDepth << '\t' << pd.viewport.maxDepth
              << '\t' << pd.scissor.offset.x << '\t' << pd.scissor.offset.y
              << '\t' << pd.scissor.extent.width << '\t' << pd.scissor.extent.height
              << '\t' << std::hex << "0x" << pd.clipCntl
              << '\t' << "0x" << pd.suScModeCntl
              << '\t' << "0x" << pd.vteCntl
              << '\t' << "0x" << pd.windowOffset << std::dec
              << '\t' << pd.vportXScale << '\t' << pd.vportXOffset
              << '\t' << pd.vportYScale << '\t' << pd.vportYOffset
              << '\t' << pd.vportZScale << '\t' << pd.vportZOffset
              << '\t' << std::hex << pd.vsHash << '\t' << pd.psHash
              << std::dec
              << "\t\t\t\t\t\t\t"   // the resolve_* columns: not a resolve
              << '\n';
            ++row;
        }
        lucent::info("draw", "per-draw diagnostic: {} rows written to {}",
                     row, diagPath);
    }
}

} // namespace gears::draw
