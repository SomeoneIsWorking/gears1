// The frame's mid-render probes. gpu_draw_probe.h says what each one answers;
// this is what each records and what each prints.

#include "gpu_draw_probe.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <format>
#include <map>
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
            lucent::info("draw", "probes PINNED to EDRAM surface {:#x}: the"
                " pixel trace samples that surface after EVERY draw, whatever is"
                " bound, so a row names the draw the change actually happened at."
                " Checkpoints still only dump when it is the bound surface",
                onlySurface);
        }
    }
    // The render comparer's staging image and buffer. Sized for the whole
    // frame's draws up front, because growing it mid-frame would reorder the
    // copies already recorded into the command buffer.
    if (const std::string& tp = lucent::config::text("DRAW_TRACE_ALL"); !tp.empty())
    {
        tracePath = tp;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ci.extent = {kThumbW, kThumbH, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkMemoryRequirements req{};
        uint32_t type = 0;
        if (vkCreateImage(R.device, &ci, nullptr, &thumbImage) == VK_SUCCESS)
        {
            vkGetImageMemoryRequirements(R.device, thumbImage, &req);
            if (!R.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
                R.FindMemory(req.memoryTypeBits, 0, type);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = type;
            if (vkAllocateMemory(R.device, &ai, nullptr, &thumbMem) != VK_SUCCESS ||
                vkBindImageMemory(R.device, thumbImage, thumbMem, 0) != VK_SUCCESS)
            {
                vkDestroyImage(R.device, thumbImage, nullptr);
                thumbImage = VK_NULL_HANDLE;
            }
        }
        const VkDeviceSize thumbBytes = VkDeviceSize(kThumbW) * kThumbH * 8;
        if (thumbImage == VK_NULL_HANDLE ||
            !R.MakeBuffer(thumbBytes * (nDraws + 2),
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT, thumbBuf, thumbBufMem, true))
        {
            lucent::warn("draw", "GEARS_DRAW_TRACE_ALL: could not create the"
                " staging image or buffer, so NO comparison file will be"
                " written and this run says nothing");
            tracePath.clear();
        }
    }
    // GEARS_DRAW_SURFACE_DUMP=<diag>[,<diag>...]: the whole surface, in its own
    // format, after each named draw. Parsed as DECIMAL because every diag index
    // in this project's tables and issue notes is decimal.
    if (const std::string& ds = lucent::config::text("DRAW_SURFACE_DUMP"); !ds.empty())
    {
        size_t at = 0;
        while (at < ds.size())
        {
            size_t comma = ds.find(',', at);
            if (comma == std::string::npos) comma = ds.size();
            const std::string one = ds.substr(at, comma - at);
            if (!one.empty())
            {
                char* end = nullptr;
                const long v = std::strtol(one.c_str(), &end, 10);
                if (end == one.c_str() || v < 0)
                    lucent::warn("draw", "GEARS_DRAW_SURFACE_DUMP: cannot parse"
                        " '{}' as a diag draw index; that entry is IGNORED and"
                        " no dump will be taken for it", one);
                else
                    dumpWanted.push_back(uint32_t(v));
            }
            at = comma + 1;
        }
        if (dumpWanted.empty())
            lucent::warn("draw", "GEARS_DRAW_SURFACE_DUMP={} named no usable"
                " diag draw index, so NOTHING will be dumped", ds);
        else
            lucent::info("draw", "surface dump armed for diag draw(s) {}"
                " -- the FULL bound surface in its own format, after the draw,"
                " before any resolve or post", ds);
    }
    // GEARS_DRAW_DEPTH_DUMP=<diag>[,<diag>...]: the depth and stencil buffers
    // after each named draw. Same decimal diag indices as the surface dump.
    if (const std::string& dd = lucent::config::text("DRAW_DEPTH_DUMP"); !dd.empty())
    {
        size_t at = 0;
        while (at < dd.size())
        {
            size_t comma = dd.find(',', at);
            if (comma == std::string::npos) comma = dd.size();
            const std::string one = dd.substr(at, comma - at);
            if (!one.empty())
            {
                char* end = nullptr;
                const long v = std::strtol(one.c_str(), &end, 10);
                if (end == one.c_str() || v < 0)
                    lucent::warn("draw", "GEARS_DRAW_DEPTH_DUMP: cannot parse"
                        " '{}' as a diag draw index; that entry is IGNORED and"
                        " no dump will be taken for it", one);
                else
                    depthWanted.push_back(uint32_t(v));
            }
            at = comma + 1;
        }
        if (depthWanted.empty())
            lucent::warn("draw", "GEARS_DRAW_DEPTH_DUMP={} named no usable diag"
                " draw index, so NOTHING will be dumped", dd);
        else
            lucent::info("draw", "depth dump armed for diag draw(s) {} -- float"
                " depth and the stencil byte, per pixel, immediately after the"
                " draw", dd);
    }
    // GEARS_DRAW_SURFACE_DUMP_PS=<16-hex ps hash>[,shading]: the twin of the
    // depth selector below. ",shading" takes the colour-WRITING half of a
    // marking/shading pair, whose coverage is what a pass that shades nothing
    // has to be measured against.
    if (const std::string& sp = lucent::config::text("DRAW_SURFACE_DUMP_PS"); !sp.empty())
    {
        const size_t comma = sp.find(',');
        const std::string hex = sp.substr(0, comma);
        char* end = nullptr;
        const uint64_t h = std::strtoull(hex.c_str(), &end, 16);
        if (end == hex.c_str() || h == 0)
            lucent::warn("draw", "GEARS_DRAW_SURFACE_DUMP_PS: cannot parse '{}'"
                " as a 16-hex pixel shader hash, so NOTHING will be dumped", hex);
        else
        {
            dumpPsHash = h;
            dumpPsShadingOnly = comma != std::string::npos &&
                                sp.find("shading", comma) != std::string::npos;
            lucent::info("draw", "surface dump armed for pixel shader {:#018x}{}"
                " -- the FULL bound surface in its own format, after each such"
                " draw", dumpPsHash,
                dumpPsShadingOnly ? " with a non-zero colour mask" : "");
        }
    }
    // GEARS_DRAW_DEPTH_DUMP_PS=<16-hex ps hash>[,marked]: aim the depth dump by
    // PIXEL SHADER rather than by diag index, because a diag index is not
    // stable between runs of this title and a dump that cannot be repeated
    // cannot be paired with a console capture. With ",marked" it takes the
    // MARKING draw of a marking/shading pair -- colour mask 0 -- which is the
    // stencil state the shading draw that follows actually tests.
    if (const std::string& dp = lucent::config::text("DRAW_DEPTH_DUMP_PS"); !dp.empty())
    {
        const size_t comma = dp.find(',');
        const std::string hex = dp.substr(0, comma);
        char* end = nullptr;
        const uint64_t h = std::strtoull(hex.c_str(), &end, 16);
        if (end == hex.c_str() || h == 0)
            lucent::warn("draw", "GEARS_DRAW_DEPTH_DUMP_PS: cannot parse '{}' as"
                " a 16-hex pixel shader hash, so NOTHING will be dumped", hex);
        else
        {
            depthPsHash = h;
            depthPsMaskedOnly = comma != std::string::npos &&
                                dp.find("marked", comma) != std::string::npos;
            lucent::info("draw", "depth dump armed for the FIRST draw of pixel"
                " shader {:#018x}{} -- float depth and the stencil byte, per"
                " pixel, immediately after it", depthPsHash,
                depthPsMaskedOnly ? " with colour mask 0 (the marking draw)" : "");
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
                            uint32_t prepIndex, const SurfaceTarget* t,
                            uint32_t surfaceBase)
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
    pixelSamples.push_back(PixelSample{drawsSoFar, prepIndex, surfaceBase, t->hostFormat});
}

// The whole surface, in its own format, after one named draw.
//
// No blit and no staging image: a blit is what costs FRAME_STEP its HDR range.
// The copy is straight out of the target, so what lands in the buffer is the
// bits the draw wrote.
void FrameProbe::DumpSurface(VkCommandBuffer cmd, uint32_t drawsSoFar,
                             uint32_t prepIndex, uint32_t diagIndex,
                             const SurfaceTarget* t, uint32_t surfaceBase,
                             uint64_t psHash, uint32_t colorMask)
{
    anyDrawSeen = true;
    if (diagIndex > highestDiag)
        highestDiag = diagIndex;
    bool wanted = std::find(dumpWanted.begin(), dumpWanted.end(), diagIndex) !=
                  dumpWanted.end();
    if (!wanted && dumpPsHash != 0 && psHash == dumpPsHash)
    {
        ++dumpPsMatches;
        if (!dumpPsShadingOnly || colorMask != 0)
        {
            wanted = true;
            ++dumpPsHashSeen;
        }
    }
    if (!wanted)
        return;
    // Already taken: a diag index can be offered twice if the caller also
    // dumps at end of frame, and two copies of one draw is not a finding.
    for (const SurfaceDump& d : dumps)
        if (d.diagIndex == diagIndex)
            return;
    if (!SurfaceWanted(surfaceBase))
    {
        lucent::warn("draw", "GEARS_DRAW_SURFACE_DUMP: diag draw {} rendered"
            " into surface {:#x}, but GEARS_DRAW_SURFACE pins the probes to"
            " {:#x}. NO dump was taken -- this is not an empty surface",
            diagIndex, surfaceBase, onlySurface);
        return;
    }
    if (!t || !t->begunThisFrame)
    {
        lucent::warn("draw", "GEARS_DRAW_SURFACE_DUMP: diag draw {} reached, but"
            " {}. NO dump was taken", diagIndex,
            t ? "its surface has not been rendered to this frame"
              : "no surface has been opened yet");
        return;
    }
    uint32_t bpp = 0;
    if (t->hostFormat == VK_FORMAT_R16G16B16A16_SFLOAT) bpp = 8;
    else if (t->hostFormat == VK_FORMAT_R8G8B8A8_UNORM) bpp = 4;
    else
    {
        // REFUSE rather than read the bytes as something they are not.
        lucent::error("draw", "GEARS_DRAW_SURFACE_DUMP: diag draw {} renders to"
            " host format {} which this probe cannot decode. NO dump was taken,"
            " and nothing here says the surface is empty",
            diagIndex, int(t->hostFormat));
        return;
    }
    const VkDeviceSize bytes = VkDeviceSize(W) * H * bpp;
    VkBuffer b = 0; VkDeviceMemory m = 0;
    if (!R.MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b, m))
    {
        lucent::error("draw", "GEARS_DRAW_SURFACE_DUMP: could not allocate the"
            " {} byte readback for diag draw {}. NO dump was taken", bytes, diagIndex);
        return;
    }
    VkBufferImageCopy rg{};
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        b, 1, &rg);
    dumps.push_back(SurfaceDump{drawsSoFar, prepIndex, diagIndex, surfaceBase,
                                t->hostFormat, b, m, bytes});
}

// The depth buffer -- depth AND stencil -- after one named draw.
//
// One buffer holds both aspects: W*H float32 depths, then W*H stencil bytes.
// Two aspects cannot share a copy region, but they can share a buffer, and one
// allocation is one thing to free.
//
// The depth image rests in DEPTH_STENCIL_ATTACHMENT_OPTIMAL between passes (see
// the mid-frame clear and the aliasing pass, which both assume it), so this
// transitions from there and puts it back.
void FrameProbe::DumpDepth(VkCommandBuffer cmd, uint32_t drawsSoFar,
                           uint32_t prepIndex, uint32_t diagIndex,
                           VkImage depthImage, VkFormat depthFormat,
                           uint64_t psHash, uint32_t colorMask)
{
    anyDrawSeen = true;
    if (diagIndex > highestDiag)
        highestDiag = diagIndex;
    bool wanted = std::find(depthWanted.begin(), depthWanted.end(), diagIndex) !=
                  depthWanted.end();
    if (!wanted && depthPsHash != 0 && psHash == depthPsHash)
    {
        ++depthPsMatches;
        if (!depthPsMaskedOnly || colorMask == 0)
        {
            // EVERY match, not the first: the pass runs more than once a frame
            // and the runs are the thing being told apart -- the mask copies
            // they feed are compared per ordinal. Each file is named by its own
            // diag index, so they cannot overwrite one another.
            wanted = true;
            ++depthPsHashSeen;
        }
    }
    if (!wanted)
        return;
    for (const DepthDump& d : depthDumps)
        if (d.diagIndex == diagIndex)
            return;
    if (depthImage == VK_NULL_HANDLE)
    {
        lucent::error("draw", "GEARS_DRAW_DEPTH_DUMP: diag draw {} reached, but"
            " this frame has no depth image. NO dump was taken", diagIndex);
        return;
    }
    // REFUSE any other format rather than decode the bytes as something they
    // are not. D32_SFLOAT_S8_UINT is what the frame creates; a change there must
    // come back here rather than silently produce nonsense.
    if (depthFormat != VK_FORMAT_D32_SFLOAT_S8_UINT)
    {
        lucent::error("draw", "GEARS_DRAW_DEPTH_DUMP: diag draw {} has depth"
            " format {}, which this probe cannot decode. NO dump was taken, and"
            " nothing here says the buffer is empty", diagIndex, int(depthFormat));
        return;
    }
    const VkDeviceSize depthBytes = VkDeviceSize(W) * H * 4;
    const VkDeviceSize bytes = depthBytes + VkDeviceSize(W) * H;
    VkBuffer b = 0; VkDeviceMemory m = 0;
    if (!R.MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b, m))
    {
        lucent::error("draw", "GEARS_DRAW_DEPTH_DUMP: could not allocate the {}"
            " byte readback for diag draw {}. NO dump was taken", bytes, diagIndex);
        return;
    }

    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = depthImage;
    bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                            0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    VkBufferImageCopy rg[2]{};
    rg[0].bufferOffset = 0;
    rg[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    rg[0].imageExtent = {W, H, 1};
    rg[1].bufferOffset = depthBytes;
    rg[1].imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
    rg[1].imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, depthImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        b, 2, rg);

    VkImageMemoryBarrier back{bar};
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr,
        1, &back);

    depthDumps.push_back(DepthDump{drawsSoFar, prepIndex, diagIndex, b, m, bytes});
}

// One thumbnail of the surface, blitted down and copied out, per draw.
void FrameProbe::TraceAll(VkCommandBuffer cmd, uint32_t drawsSoFar,
                          uint32_t prepIndex, const SurfaceTarget* t,
                          uint32_t surfaceBase)
{
    if (tracePath.empty() || !t || !t->begunThisFrame)
        return;
    if (!SurfaceWanted(surfaceBase))
        return;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = thumbImage;
    b.subresourceRange = range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    // NEAREST, so the values are the surface's own rather than an average of
    // them: a comparer that filters cannot tell a changed pixel from a moved one.
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {int32_t(W), int32_t(H), 1};
    bl.dstOffsets[1] = {int32_t(kThumbW), int32_t(kThumbH), 1};
    vkCmdBlitImage(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        thumbImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    VkImageMemoryBarrier r = b;
    r.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    r.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    r.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    r.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &r);
    VkBufferImageCopy rg{};
    rg.bufferOffset = VkDeviceSize(thumbs.size()) * kThumbW * kThumbH * 8;
    rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.imageExtent = {kThumbW, kThumbH, 1};
    vkCmdCopyImageToBuffer(cmd, thumbImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        thumbBuf, 1, &rg);
    thumbs.push_back(ThumbSample{drawsSoFar, prepIndex, surfaceBase});
}

// Decodes each dump to float, writes it as a PFM, and prints what it holds.
//
// THE NEGATIVE IS DESIGNED FIRST. Every line carries its denominator, and a
// diag index that was asked for and never taken is reported by name against the
// highest index the frame reached -- because "the surface was empty" and "you
// named a draw this frame never issued" are the two readings this project has
// repeatedly confused, and silence looks like the first one.
void FrameProbe::ReportSurfaceDumps(const std::vector<PreparedDraw>& prepared)
{
    if (!Dumping())
        return;
    const std::string& dirStr = lucent::config::text("DRAW_DIR");
    const std::filesystem::path outDir = dirStr.empty()
        ? std::filesystem::path("scratch/screenshots")
        : std::filesystem::path(dirStr);
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    for (const SurfaceDump& d : dumps)
    {
        void* p = nullptr;
        if (vkMapMemory(R.device, d.mem, 0, d.bytes, 0, &p) != VK_SUCCESS)
        {
            lucent::error("draw", "GEARS_DRAW_SURFACE_DUMP: diag draw {} was"
                " copied but its readback could not be mapped. NO reading",
                d.diagIndex);
            continue;
        }
        std::vector<uint8_t> raw(size_t(d.bytes));
        std::memcpy(raw.data(), p, size_t(d.bytes));
        vkUnmapMemory(R.device, d.mem);

        const size_t px = size_t(W) * H;
        // ALL FOUR CHANNELS. Alpha is not decoration here: the debug shader
        // writes a sentinel into it so a dump carries its own coverage mask,
        // and a three-channel dump would throw away the denominator that three
        // retracted measurements were missing.
        std::vector<float> rgba(px * 4);
        for (size_t k = 0; k < px; ++k)
            for (int c = 0; c < 4; ++c)
            {
                float v;
                if (d.format == VK_FORMAT_R8G8B8A8_UNORM)
                    v = float(raw[k * 4 + size_t(c)]) / 255.0f;
                else
                {
                    uint16_t h;
                    std::memcpy(&h, raw.data() + k * 8 + size_t(c) * 2, 2);
                    v = HalfToFloat(h);
                }
                rgba[k * 4 + size_t(c)] = v;
            }

        double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
        double sum[3] = {0, 0, 0};
        uint64_t nonZero = 0, above1 = 0;
        for (size_t k = 0; k < px; ++k)
        {
            bool nz = false;
            for (int c = 0; c < 3; ++c)
            {
                const double v = rgba[k * 4 + size_t(c)];
                if (v < mn[c]) mn[c] = v;
                if (v > mx[c]) mx[c] = v;
                sum[c] += v;
                if (v != 0.0) nz = true;
                if (v > 1.0) { above1 += 1; break; }
            }
            if (nz) ++nonZero;
        }

        // NumPy .npy, float32, shape (H, W, 4). Chosen over PFM because PFM is
        // three-channel and alpha carries the coverage sentinel, and over PNG
        // because an 8-bit image cannot hold what an HDR surface is dumped to
        // show. The header is 60 bytes of ASCII; np.load reads it with no
        // helper, which is the whole point -- an analysis that needs a bespoke
        // reader is one nobody writes.
        const std::string name =
            std::format("surface_{:x}_after_diag{}.npy", d.surface, d.diagIndex);
        const std::filesystem::path path = outDir / name;
        bool wrote = false;
        if (std::ofstream f(path, std::ios::binary); f)
        {
            std::string hdr = std::format(
                "{{'descr': '<f4', 'fortran_order': False, 'shape': ({}, {}, 4), }}",
                H, W);
            size_t total = 10 + hdr.size() + 1;
            while (total % 64 != 0) { hdr += ' '; ++total; }
            hdr += '\n';
            const uint16_t hlen = uint16_t(hdr.size());
            f.write("\x93NUMPY\x01\x00", 8);
            f.write(reinterpret_cast<const char*>(&hlen), 2);
            f.write(hdr.data(), std::streamsize(hdr.size()));
            f.write(reinterpret_cast<const char*>(rgba.data()),
                    std::streamsize(sizeof(float) * rgba.size()));
            wrote = f.good();
        }

        const PreparedDraw* by =
            d.prepIndex < prepared.size() ? &prepared[d.prepIndex] : nullptr;
        lucent::Line l;
        l.add("surface dump after DIAG draw {}", d.diagIndex);
        if (by)
            l.add(" (ps {:#x}, issued draw {})", by->psHash, d.draws);
        l.add(", surface {:#x}, {}x{} {}:", d.surface, W, H,
              d.format == VK_FORMAT_R8G8B8A8_UNORM ? "R8G8B8A8" : "R16G16B16A16_SFLOAT");
        for (int c = 0; c < 3; ++c)
            l.add("\n  {}  min {:.5f}  max {:.5f}  mean {:.5f}",
                  "RGB"[c], mn[c], mx[c], sum[c] / double(px));
        l.add("\n  {} of {} px non-zero [{:.1f}%], {} px with a channel above 1.0",
              nonZero, px, 100.0 * double(nonZero) / double(px), above1);
        l.add("\n  -> {}", wrote ? path.string()
                                 : std::string("NOT WRITTEN (the statistics above"
                                               " are still this draw's output)"));
        l.flush(lucent::Level::Info, "draw");
    }

    // Asked for and never taken. Named individually: a list of three indices of
    // which one silently produced nothing is exactly the shape of the reading
    // that cost catalog #77 a session.
    if (dumpPsHash != 0)
    {
        // Both denominators, because "the shader never ran" and "it ran but
        // never with a colour mask" are different failures.
        if (dumpPsHashSeen == 0)
            lucent::warn("draw", "GEARS_DRAW_SURFACE_DUMP_PS={:016x}: NO dump."
                " The frame ran {} draw(s) of that pixel shader, {} of them"
                " matching the colour-mask rule{}, over {} draws reaching diag"
                " index {}. That is 'the draw this asked for did not happen',"
                " NOT 'the surface was empty'", dumpPsHash, dumpPsMatches,
                dumpPsHashSeen,
                dumpPsShadingOnly ? " (non-zero colour mask)" : " (any mask)",
                prepared.size(), highestDiag);
        else
            lucent::info("draw", "GEARS_DRAW_SURFACE_DUMP_PS={:016x}: {} draw(s)"
                " of that shader in the frame, {} matched the mask rule and ALL"
                " of them were dumped", dumpPsHash, dumpPsMatches, dumpPsHashSeen);
    }

    for (const uint32_t want : dumpWanted)
    {
        bool taken = false;
        for (const SurfaceDump& d : dumps)
            if (d.diagIndex == want) { taken = true; break; }
        if (taken)
            continue;
        if (!anyDrawSeen)
            lucent::warn("draw", "GEARS_DRAW_SURFACE_DUMP={}: NO dump, and no"
                " draw was offered to this probe at all. The frame issued"
                " nothing, so this says nothing about diag draw {}", want, want);
        else
            lucent::warn("draw", "GEARS_DRAW_SURFACE_DUMP={}: NO dump was taken."
                " The frame's draws ran to diag index {} ({} prepared entries),"
                " so this is 'that draw was never offered', NOT 'that draw wrote"
                " nothing'. A warning above says why if it was offered and"
                " refused", want, highestDiag, prepared.size());
    }
}

// Decodes each depth dump, writes it as a .npy and prints what it holds.
//
// THE NEGATIVE IS DESIGNED FIRST, as in ReportSurfaceDumps. Two things beyond
// min/max/mean are printed because they are what the readings this probe exists
// to replace could not see:
//
//   * the MOST COMMON depth value and its share, so "the buffer still holds the
//     clear" is a number rather than an inference. A mean says nothing here: a
//     far-cleared buffer with a near object in it and a uniformly mid-filled one
//     can share a mean.
//   * EVERY stencil value present, with its pixel count. Stencil has 256 states
//     and the whole list is a few entries, so there is no sampling and no cap to
//     hide the interesting case behind.
void FrameProbe::ReportDepthDumps(const std::vector<PreparedDraw>& prepared)
{
    if (!DumpingDepth())
        return;
    const std::string& dirStr = lucent::config::text("DRAW_DIR");
    const std::filesystem::path outDir = dirStr.empty()
        ? std::filesystem::path("scratch/screenshots")
        : std::filesystem::path(dirStr);
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    const size_t px = size_t(W) * H;
    for (const DepthDump& d : depthDumps)
    {
        void* p = nullptr;
        if (vkMapMemory(R.device, d.mem, 0, d.bytes, 0, &p) != VK_SUCCESS)
        {
            lucent::error("draw", "GEARS_DRAW_DEPTH_DUMP: diag draw {} was copied"
                " but its readback could not be mapped. NO reading", d.diagIndex);
            continue;
        }
        std::vector<uint8_t> raw(size_t(d.bytes));
        std::memcpy(raw.data(), p, size_t(d.bytes));
        vkUnmapMemory(R.device, d.mem);

        // Channel 0 is depth, channel 1 the stencil byte as a raw 0..255 value
        // -- not normalised, because a stencil value is an identifier and
        // dividing it by 255 makes it a number nobody can compare to the guest's
        // reference value.
        std::vector<float> out(px * 2);
        double mn = 1e30, mx = -1e30, sum = 0;
        std::map<uint32_t, uint64_t> depthHist;   // quantised to 24 bits
        std::array<uint64_t, 256> stencilHist{};
        for (size_t k = 0; k < px; ++k)
        {
            float dv;
            std::memcpy(&dv, raw.data() + k * 4, 4);
            const uint8_t sv = raw[size_t(W) * H * 4 + k];
            out[k * 2] = dv;
            out[k * 2 + 1] = float(sv);
            if (dv < mn) mn = dv;
            if (dv > mx) mx = dv;
            sum += dv;
            const float c = dv < 0.0f ? 0.0f : (dv > 1.0f ? 1.0f : dv);
            ++depthHist[uint32_t(c * 16777215.0f + 0.5f)];
            ++stencilHist[sv];
        }
        uint32_t modeBits = 0; uint64_t modeCount = 0;
        for (const auto& [bits, n] : depthHist)
            if (n > modeCount) { modeCount = n; modeBits = bits; }

        const std::string name =
            std::format("depth_after_diag{}.npy", d.diagIndex);
        const std::filesystem::path path = outDir / name;
        bool wrote = false;
        if (std::ofstream f(path, std::ios::binary); f)
        {
            std::string hdr = std::format(
                "{{'descr': '<f4', 'fortran_order': False, 'shape': ({}, {}, 2), }}",
                H, W);
            size_t total = 10 + hdr.size() + 1;
            while (total % 64 != 0) { hdr += ' '; ++total; }
            hdr += '\n';
            const uint16_t hlen = uint16_t(hdr.size());
            f.write("\x93NUMPY\x01\x00", 8);
            f.write(reinterpret_cast<const char*>(&hlen), 2);
            f.write(hdr.data(), std::streamsize(hdr.size()));
            f.write(reinterpret_cast<const char*>(out.data()),
                    std::streamsize(sizeof(float) * out.size()));
            wrote = f.good();
        }

        const PreparedDraw* by =
            d.prepIndex < prepared.size() ? &prepared[d.prepIndex] : nullptr;
        lucent::Line l;
        l.add("depth dump after DIAG draw {}", d.diagIndex);
        if (by)
            l.add(" (ps {:#x}, issued draw {})", by->psHash, d.draws);
        l.add(", {}x{}:", W, H);
        l.add("\n  depth    min {:.6f}  max {:.6f}  mean {:.6f}  distinct {}",
              mn, mx, sum / double(px), depthHist.size());
        l.add("\n  most common depth {:.6f} over {} of {} px [{:.1f}%]",
              double(modeBits) / 16777215.0, modeCount, px,
              100.0 * double(modeCount) / double(px));
        l.add("\n  stencil  ");
        uint32_t values = 0;
        for (uint32_t s = 0; s < 256; ++s)
            if (stencilHist[s])
            {
                l.add("{}{:#04x}={} [{:.1f}%]", values++ ? ", " : "", s,
                      stencilHist[s],
                      100.0 * double(stencilHist[s]) / double(px));
            }
        l.add("  ({} value(s) present, all of them listed)", values);
        l.add("\n  -> {}", wrote ? path.string()
                                 : std::string("NOT WRITTEN (the statistics above"
                                               " are still this draw's depth)"));
        l.flush(lucent::Level::Info, "draw");
    }

    if (depthPsHash != 0)
    {
        // The negative, with its denominators: "no draw ran that shader" and
        // "the shader ran but never with the mask this asked for" are different
        // failures and were repeatedly read as each other.
        if (depthPsHashSeen == 0)
            lucent::warn("draw", "GEARS_DRAW_DEPTH_DUMP_PS={:016x}: NO dump."
                " The frame ran {} draw(s) of that pixel shader, {} of them"
                " matching the colour-mask rule{}, over {} draws reaching diag"
                " index {}. That is 'the draw this asked for did not happen',"
                " NOT 'the depth buffer was empty'",
                depthPsHash, depthPsMatches, depthPsHashSeen,
                depthPsMaskedOnly ? " (colour mask 0)" : " (any mask)",
                prepared.size(), highestDiag);
        else
            lucent::info("draw", "GEARS_DRAW_DEPTH_DUMP_PS={:016x}: {} draw(s)"
                " of that shader in the frame, {} matched the mask rule and"
                " ALL of them were dumped", depthPsHash, depthPsMatches,
                depthPsHashSeen);
    }

    for (const uint32_t want : depthWanted)
    {
        bool taken = false;
        for (const DepthDump& d : depthDumps)
            if (d.diagIndex == want) { taken = true; break; }
        if (taken)
            continue;
        if (!anyDrawSeen)
            lucent::warn("draw", "GEARS_DRAW_DEPTH_DUMP={}: NO dump, and no draw"
                " was offered to this probe at all. The frame issued nothing, so"
                " this says nothing about diag draw {}", want, want);
        else
            lucent::warn("draw", "GEARS_DRAW_DEPTH_DUMP={}: NO dump was taken."
                " The frame's draws ran to diag index {} ({} prepared entries),"
                " so this is 'that draw was never offered', NOT 'the depth buffer"
                " was empty'. A message above says why if it was offered and"
                " refused", want, highestDiag, prepared.size());
    }
}

void FrameProbe::Report(const std::vector<PreparedDraw>& prepared)
{
    ReportSurfaceDumps(prepared);
    ReportDepthDumps(prepared);
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
            // The PREPARED INDEX of the draw just issued, recorded at sample
            // time. Deriving it from the draw count instead -- prepared[n-1] --
            // is wrong wherever a resolve has gone past, because `drawn` counts
            // draws and `prepared` also holds resolves; that named a draw three
            // rows early and sent catalog #62 after a colour-masked draw that
            // was never the writer.
            const uint32_t pi = pixelSamples[i].prepIndex;
            const PreparedDraw* by = pi < prepared.size() ? &prepared[pi] : nullptr;
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
    // The render comparer's file. One row per draw: what the draw WAS, and a
    // hash plus statistics of the surface AFTER it. Two runs give two files and
    // tools/render_diff.py names the first row that differs.
    if (!tracePath.empty())
    {
        std::ofstream tf(tracePath);
        if (!tf)
        {
            lucent::error("draw", "GEARS_DRAW_TRACE_ALL: cannot write {}."
                " Nothing was recorded", tracePath);
        }
        else
        {
            void* p = nullptr;
            const size_t stride = size_t(kThumbW) * kThumbH * 4;
            const VkDeviceSize bytes = VkDeviceSize(stride) * 2 * thumbs.size();
            std::vector<uint16_t> all(stride * thumbs.size());
            if (!thumbs.empty() &&
                vkMapMemory(R.device, thumbBufMem, 0, bytes, 0, &p) == VK_SUCCESS)
            {
                std::memcpy(all.data(), p, size_t(bytes));
                vkUnmapMemory(R.device, thumbBufMem);
            }
            tf << "draw\tdiag\tsurface\tps_hash\tthumb_hash"
                  "\tmaxR\tmaxG\tmaxB\tmeanR\tmeanG\tmeanB\n";
            for (size_t i = 0; i < thumbs.size(); ++i)
            {
                const uint32_t n = thumbs[i].draws;
                const uint32_t pi = thumbs[i].prepIndex;
                const PreparedDraw* by = pi < prepared.size() ? &prepared[pi] : nullptr;
                double mx[3] = {-1e30, -1e30, -1e30}, sum[3] = {0, 0, 0};
                uint64_t hash = 1469598103934665603ull;   // FNV-1a
                for (size_t k = 0; k < size_t(kThumbW) * kThumbH; ++k)
                {
                    for (int c = 0; c < 4; ++c)
                    {
                        const uint16_t bits = all[i * stride + k * 4 + c];
                        hash = (hash ^ bits) * 1099511628211ull;
                        if (c < 3)
                        {
                            const float v = HalfToFloat(bits);
                            if (v > mx[c]) mx[c] = v;
                            sum[c] += v;
                        }
                    }
                }
                const double px = double(kThumbW) * kThumbH;
                tf << n << '\t' << (by ? int64_t(by->diagIndex) : -1)
                   << '\t' << std::hex << "0x" << thumbs[i].surface << std::dec
                   << '\t' << std::hex << (by ? by->psHash : 0) << std::dec
                   << '\t' << std::hex << hash << std::dec
                   << '\t' << mx[0] << '\t' << mx[1] << '\t' << mx[2]
                   << '\t' << sum[0] / px << '\t' << sum[1] / px
                   << '\t' << sum[2] / px << '\n';
            }
            lucent::info("draw", "render comparer: {} row(s) written to {}"
                " ({}x{} thumbnails). Diff two of these with"
                " tools/render_diff.py", thumbs.size(), tracePath,
                kThumbW, kThumbH);
        }
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
    for (const SurfaceDump& d : dumps)
    {
        vkDestroyBuffer(R.device, d.buffer, nullptr);
        vkFreeMemory(R.device, d.mem, nullptr);
    }
    dumps.clear();
    for (const DepthDump& d : depthDumps)
    {
        vkDestroyBuffer(R.device, d.buffer, nullptr);
        vkFreeMemory(R.device, d.mem, nullptr);
    }
    depthDumps.clear();
}


void DrawStats::Build(VkCommandBuffer cmd, size_t nDraws)
{
    const std::string& dp = lucent::config::text("DRAW_DIAG");
    diagPath = dp;
    if (const std::string& cw = lucent::config::text("DRAW_CLIP_WATCH"); !cw.empty())
    {
        char* end = nullptr;
        clipWatchHash = std::strtoull(cw.c_str(), &end, 16);
        if (end == cw.c_str() || clipWatchHash == 0)
        {
            lucent::warn("draw", "GEARS_DRAW_CLIP_WATCH: cannot parse '{}' as a"
                " 16-hex vertex shader hash; NO clip watch is running", cw);
            clipWatchHash = 0;
        }
        else if (diagPath.empty())
            lucent::warn("draw", "GEARS_DRAW_CLIP_WATCH needs GEARS_DRAW_DIAG:"
                " the statistics it reads are gathered for the table. NO clip"
                " watch is running");
        else
        {
            // <hash>[:<min primitives>[:<max percent kept>]]
            if (*end == ':')
            {
                clipWatchMinPrims = uint32_t(std::strtoul(end + 1, &end, 10));
                if (*end == ':')
                    clipWatchMaxKeptPct = uint32_t(std::strtoul(end + 1, &end, 10));
            }
            lucent::info("draw", "clip watch armed on vertex shader {:016x}:"
                " a frame is THE ONE when a draw of it assembles at least {}"
                " primitives and keeps at most {}% of them. Reported EVERY"
                " frame, fired or not", clipWatchHash, clipWatchMinPrims,
                clipWatchMaxKeptPct);
        }
    }
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

// PROCESS-WIDE, because the probe and its DrawStats are rebuilt every frame --
// a member latch is reset by the next frame and the evidence is destroyed by
// the frame after the one that produced it.
bool g_clipWatchHeld = false;

void DrawStats::WriteTable(uint32_t drawn, const std::vector<PreparedDraw>& prepared,
                           const std::vector<uint64_t>& st)
{
    // THE VERDICT IS COMPUTED EVERY FRAME EVEN WHEN THE TABLE IS HELD. It used
    // to return here, so the watch printed its per-frame line exactly once --
    // for the frame it fired on -- while claiming to report every frame. That
    // is what made a one-off firing look like a permanent condition. The rows
    // are computed either way and thrown away when held; only the FILE is
    // frozen, which is all the hold was ever for.
    const bool wasHeld = g_clipWatchHeld;
    if (wasHeld)
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            lucent::info("draw", "clip watch fired: HOLDING {} as it stood on"
                " that frame. Later frames are still rendered and still"
                " reported, but no longer overwrite it", diagPath);
        }
    }
    std::error_code ec;
    const std::filesystem::path dp(diagPath);
    std::ofstream file;
    std::ostringstream discard;
    if (!wasHeld)
    {
        if (dp.has_parent_path())
            std::filesystem::create_directories(dp.parent_path(), ec);
        file.open(dp, std::ios::binary);
    }
    std::ostream& t = wasHeld ? static_cast<std::ostream&>(discard)
                              : static_cast<std::ostream&>(file);
    if (!wasHeld && !file)
    {
        lucent::error("draw", "per-draw diagnostic: cannot write {}", diagPath);
    }
    else
    {
        const std::string header =
            "draw\tsurface\tcolor_fmt\tcolor_exp_bias\tedram_mode\tprim\tprim_name"
             "\tindexed\tcount\tfrag_stage\tia_verts\tia_prims"
             "\tprims_after_clip\tfrag_invocations\tverdict"
             "\tdepth_test\tdepth_write\tdepth_func"
             // THE DEPTH BUFFER AND THE STENCIL TEST, which decide coverage as
             // firmly as the scissor and were the one thing this table could
             // not show. A shadow pass MARKS stencil and the pass that reads
             // the marks runs segments later, so "which depth+stencil memory,
             // and what test" is the question a mask that covers the wrong
             // pixels turns into -- and answering it needed a rebuild rather
             // than a re-read (catalog #91). depth_base is RB_DEPTH_INFO's, as
             // the GUEST programmed it, whatever GEARS_DRAW_SPLIT_DEPTH does
             // with it.
             "\tdepth_base\tstencil_on\tstencil_func\tstencil_ref\tstencil_mask"
             "\tstencil_fail\tstencil_zpass\tstencil_zfail"
             // THE BACK-FACE SET, because a depth-fail shadow volume is
             // defined by the two faces disagreeing: this title's light
             // volumes set backface_enable and give the back faces
             // DECREMENT_WRAP against the front faces' INCREMENT_WRAP, and a
             // table showing only the front set describes half a mechanism.
             "\tstencil_bf_on\tstencil_bf_func\tstencil_bf_fail"
             "\tstencil_bf_zpass\tstencil_bf_zfail"
             // THE RAW REGISTERS TOO, because the cross-emulator comparison
             // keys on them: tools/pass_draws.py counts draws per
             // (vertex shader, pixel shader, depth control, stencil ref/mask,
             // blend) and a derived column cannot be joined against the
             // console's record of the register itself.
             "\tdepth_control\tstencil_ref_mask_raw"
             "\tcolor_mask"
             "\tblend_on\tblend0\tvp_x\tvp_y\tvp_w\tvp_h\tvp_minz"
             "\tvp_maxz\tclip_disable\tsc_x\tsc_y\tsc_w\tsc_h"
             "\tclip_cntl\tsu_sc_mode\tvte_cntl\twindow_offset"
             "\tsurface_pitch\tmsaa"
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
             // The DESTINATION FORMAT, which is how tools/layer_compare.py names
             // a pass ("srcC2D0 1280x720 f7 #0"). Without it here, mapping one
             // of that tool's rows back to the draw that produced it means
             // going through resolve-dump FILENAMES, and the two instruments
             // that are meant to be read together cannot be joined.
             "\tresolve_swap_rb\tresolve_scale\tcopy_sample_sel"
             // BOTH ends of the copy's format question. The destination format
             // is how layer_compare.py names a pass; the SOURCE format is
             // RB_COLOR_INFO[copy_src_select], which is what decides whether the
             // EDRAM reinterpretation pass fires before the copy -- and one
             // conversion it fires accounts for the whole of catalog #95.
             "\tresolve_dest_fmt\tresolve_src_fmt\n";
        t << header;
        // THE HEADER'S OWN WIDTH, counted from the string just written rather
        // than kept in a second place that can drift from it. Both row emitters
        // are built against this, and a mismatch is reported instead of writing
        // a row nobody can read by column.
        const size_t kDiagColumns = size_t(std::count(header.begin(),
                                                      header.end(), '\t')) + 1;
        uint32_t row = 0;
        for (const PreparedDraw& pd : prepared)
        {
            // Resolves are rows too. They are observable boundaries between
            // render-target phases, so omitting them flattens the frame into a
            // draw stream with no handoff points. Columns a resolve cannot
            // populate stay empty rather than reading as zero-valued draw data.
            if (pd.isResolve)
            {
                // PLACED BY COLUMN, NOT BY COUNTING TABS. The previous version
                // emitted runs of separators and drifted: the rect landed one
                // column left of vp_x and the row ended at 65 fields against
                // the header's 71, so every column-indexed read of a resolve
                // row was wrong. Catalog #86 built a conclusion on one and had
                // to retract it. Indexing by column makes that impossible, and
                // the width is asserted below rather than trusted.
                std::vector<std::string> f(kDiagColumns);
                auto hex = [](uint32_t v) {
                    std::ostringstream o; o << "0x" << std::hex << v; return o.str(); };
                auto dec = [](auto v) { return std::to_string(v); };
                f[0]  = dec(pd.diagIndex);
                f[1]  = hex(pd.surfaceBase);
                f[2]  = dec(pd.colorFormat);
                f[3]  = dec(pd.colorExpBias);
                f[4]  = dec(pd.edramMode);
                f[6]  = "resolve";                       // prim_name
                f[14] = "resolve";                       // verdict
                // The copy's rectangle, in the viewport columns by design: a
                // resolve programs no viewport and the rect is the geometry it
                // does have.
                f[36] = dec(pd.resolveSrcRect.offset.x);        // vp_x
                f[37] = dec(pd.resolveSrcRect.offset.y);        // vp_y
                f[38] = dec(pd.resolveSrcRect.extent.width);    // vp_w
                f[39] = dec(pd.resolveSrcRect.extent.height);   // vp_h
                // The copy's rectangle is in PIXELS and its source is an EDRAM
                // surface addressed in SAMPLES, so these two say which part of
                // it the copy reads (catalog #91).
                f[51] = dec(pd.surfaceInfo & 0x3FFF);           // surface_pitch
                f[52] = dec((pd.surfaceInfo >> 16) & 3);        // msaa
                f[61] = hex(pd.resolveDest);
                f[62] = dec(pd.resolveSrcRect.extent.width) + "x" +
                        dec(pd.resolveSrcRect.extent.height);
                f[63] = dec(pd.resolveDstX) + "," + dec(pd.resolveDstY);
                f[64] = dec(pd.resolveIsDepth ? 1 : 0);
                f[65] = dec(pd.clearsDepth ? 1 : 0);
                f[66] = dec(pd.resolveSwapRB ? 1 : 0);
                f[67] = dec(pd.resolveScale);
                f[68] = dec(pd.resolveSampleSelect);
                f[69] = dec(pd.resolveDestFormat);
                f[70] = dec(pd.resolveSrcFormat);
                for (size_t i = 0; i < f.size(); ++i)
                    t << (i ? "\t" : "") << f[i];
                t << '\n';
                continue;
            }
            if (row >= drawn)
            { ++row; continue; }
            const uint64_t* s = &st[size_t(row) * kCounters];
            // The verdict is the whole point: it says which stage
            // this draw died at, in the vocabulary the pipeline
            // statistics can actually support.
            // GEARS_DRAW_CLIP_WATCH: does THIS frame contain a draw of a named
            // vertex shader that assembled primitives and then lost every one of
            // them to clipping? The frame gate cannot ask this -- it decides
            // before the frame renders and prims_after_clip only exists after --
            // so a run aimed at that failure could only hope to catch it. Made
            // observable per frame here; the harness watches for it and stops.
            if (clipWatchHash != 0 && pd.vsHash == clipWatchHash)
            {
                ++clipWatchDraws;
                // Every watched draw is named with BOTH numbers, so a frame
                // that is not the one still says what it saw. A bare count of
                // draws that lost everything cannot distinguish the reference's
                // own behaviour from ours.
                {
                    std::ostringstream d;
                    if (!clipWatchDetail.empty()) d << ", ";
                    d << pd.diagIndex << " assembled " << s[1]
                      << " kept " << s[2];
                    clipWatchDetail += d.str();
                }
                if (s[1] >= clipWatchMinPrims &&
                    s[2] * 100 <= uint64_t(clipWatchMaxKeptPct) * s[1])
                    ++clipWatchKilled;
            }
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
              // RB_DEPTHCONTROL: stencil_enable bit 0, stencilfunc bits 8-10,
              // and the three ops at 11-13 (fail), 14-16 (ZPASS), 17-19 (ZFAIL)
              // -- that order, per registers.h, and NOT fail/zfail/zpass, which
              // is the order these columns were labelled with until it sent
              // catalog #91 chasing a stencil bug that did not exist. The
              // labels said a light volume marked on ZPASS; the register says
              // it marks on ZFAIL with backface_enable set, which is a
              // depth-fail shadow volume, and a back face decrementing from 0
              // is where the 0xff came from.
              // RB_STENCILREFMASK: ref bits 0-7, mask bits 8-15.
              << '\t' << std::hex << "0x" << pd.guestDepthBase << std::dec
              << '\t' << (pd.depthControl & 1)
              << '\t' << ((pd.depthControl >> 8) & 7)
              << '\t' << (pd.stencilRefMask & 0xFF)
              << '\t' << ((pd.stencilRefMask >> 8) & 0xFF)
              << '\t' << ((pd.depthControl >> 11) & 7)
              << '\t' << ((pd.depthControl >> 14) & 7)
              << '\t' << ((pd.depthControl >> 17) & 7)
              << '\t' << ((pd.depthControl >> 7) & 1)
              << '\t' << ((pd.depthControl >> 20) & 7)
              << '\t' << ((pd.depthControl >> 23) & 7)
              << '\t' << ((pd.depthControl >> 26) & 7)
              << '\t' << ((pd.depthControl >> 29) & 7)
              << '\t' << std::hex << "0x" << pd.depthControl
              << '\t' << "0x" << pd.stencilRefMask << std::dec
              << '\t' << (pd.colorMask & 0xF)
              << '\t' << (BlendIsIdentity(pd.blend0) ? 0 : 1)
              << '\t' << std::hex << "0x" << pd.blend0 << std::dec
              << '\t' << pd.viewport.x << '\t' << pd.viewport.y
              << '\t' << pd.viewport.width << '\t' << pd.viewport.height
              << '\t' << pd.viewport.minDepth << '\t' << pd.viewport.maxDepth
              << '\t' << (pd.clipDisable ? 1 : 0)
              << '\t' << pd.scissor.offset.x << '\t' << pd.scissor.offset.y
              << '\t' << pd.scissor.extent.width << '\t' << pd.scissor.extent.height
              << '\t' << std::hex << "0x" << pd.clipCntl
              << '\t' << "0x" << pd.suScModeCntl
              << '\t' << "0x" << pd.vteCntl
              << '\t' << "0x" << pd.windowOffset << std::dec
              << '\t' << (pd.surfaceInfo & 0x3FFF)
              << '\t' << ((pd.surfaceInfo >> 16) & 3)
              << '\t' << pd.vportXScale << '\t' << pd.vportXOffset
              << '\t' << pd.vportYScale << '\t' << pd.vportYOffset
              << '\t' << pd.vportZScale << '\t' << pd.vportZOffset
              << '\t' << std::hex << pd.vsHash << '\t' << pd.psHash
              << std::dec
              << "\t\t\t\t\t\t\t\t\t\t"   // the resolve_* columns: not a resolve
              << '\n';
            ++row;
        }
        if (wasHeld)
            lucent::info("draw", "per-draw diagnostic: {} rows computed and"
                " DISCARDED, {} is held from the frame that fired", row, diagPath);
        else
            lucent::info("draw", "per-draw diagnostic: {} rows written to {}",
                         row, diagPath);
        // The clip watch's per-frame verdict, printed whether or not it fired.
        // A frame that does NOT contain the failure has to say so with its
        // denominator, or a harness waiting for the line cannot tell "not this
        // frame" from "the watch is not running".
        if (clipWatchHash != 0)
            lucent::info("draw", "CLIP WATCH {:016x}: {} draw(s) of that vertex"
                " shader this frame, {} of them assembled at least {} primitives"
                " and kept at most {}% [{}]{}",
                clipWatchHash, clipWatchDraws, clipWatchKilled,
                clipWatchMinPrims, clipWatchMaxKeptPct,
                clipWatchDraws ? clipWatchDetail : std::string("no draw of it"),
                clipWatchKilled ? " <-- THIS FRAME IS THE ONE" : "");
        if (clipWatchKilled && !wasHeld)
            g_clipWatchHeld = true;
        clipWatchDraws = clipWatchKilled = 0;
        clipWatchDetail.clear();
    }
}

} // namespace gears::draw
