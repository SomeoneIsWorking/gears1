// The two resolve dispatches. gpu_draw_targets.h says what a resolve target is;
// this is how the pixels get into one.
//
// A surface's colour image leaves a render pass in TRANSFER_SRC_OPTIMAL, so a
// resolve is: end the pass -> dispatch into the destination's own host image ->
// begin the next pass with LOAD.
//
// The dispatch covers the WHOLE surface rather than the resolve rectangle. That
// is consistent with how tiles are handled here: the console resolves once per
// predicated tile, but our surface target already holds every tile accumulated
// into one full-size image, so each tile's resolve carries the whole (correct)
// surface and the last one leaves the destination right. It follows the tile
// model rather than approximating around it.

#include "gpu_draw_targets.h"

#include <algorithm>
#include <string>

#include <lucent/config.h>
#include <lucent/log.h>

#include "gpu_draw_xlate.h"

namespace gears::draw
{

void RenderTargetCache::ResolveDepthTo(VkCommandBuffer cmd, ResolveTarget& dst,
                                       const VkRect2D& srcRect, int32_t dstX,
                                       int32_t dstY, bool isFloat24,
                                       const ResolveSampling& smp)
{
    // THE RECTANGLE IS IN PIXELS, THE IMAGE IN SAMPLES. W and H are the sample
    // grid, so the rectangle is clamped to the PIXEL extent this source has
    // under its own sample scale -- clamping a pixel rectangle to a sample
    // extent would let a 2X copy ask for twice the rows the surface holds.
    const int32_t pw = int32_t(W / std::max(1u, smp.scaleX));
    const int32_t ph = int32_t(H / std::max(1u, smp.scaleY));
    const int32_t sx0 = std::clamp<int32_t>(srcRect.offset.x, 0, pw);
    const int32_t sy0 = std::clamp<int32_t>(srcRect.offset.y, 0, ph);
    int32_t sx1 = std::clamp<int32_t>(srcRect.offset.x + int32_t(srcRect.extent.width),
                                      sx0, pw);
    int32_t sy1 = std::clamp<int32_t>(srcRect.offset.y + int32_t(srcRect.extent.height),
                                      sy0, ph);
    const int32_t dw = int32_t(dst.width), dh = int32_t(dst.imageHeight);
    if (dstX >= dw || dstY >= dh)
        return;
    sx1 = std::min<int32_t>(sx1, sx0 + std::max<int32_t>(0, dw - dstX));
    sy1 = std::min<int32_t>(sy1, sy0 + std::max<int32_t>(0, dh - dstY));
    if (sx1 <= sx0 || sy1 <= sy0)
        return;

    VkImageMemoryBarrier pre[2]{};
    pre[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    pre[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    pre[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    pre[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    pre[0].srcQueueFamilyIndex = pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[0].image = P.depth;
    // BOTH aspects: the image is D32_SFLOAT_S8_UINT, and a layout transition
    // must name every aspect the image has.
    pre[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT |
                               VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    pre[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    pre[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pre[1].oldLayout = dst.everWritten ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_UNDEFINED;
    pre[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre[1].srcQueueFamilyIndex = pre[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[1].image = dst.image;
    pre[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre);

    VkDescriptorSet set = resolveDepthSets[resolveDepthSetsUsed++];
    VkDescriptorImageInfo srcInfo{VK_NULL_HANDLE, P.depthSampledView,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, dst.storageView,
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &srcInfo;
    w[1] = w[0];
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &dstInfo;
    vkUpdateDescriptorSets(R.device, 2, w, 0, nullptr);

    draw::ResolvePushConstants pc{};
    // srcOffset is in SAMPLES, extent in destination PIXELS.
    pc.srcOffset[0] = sx0 * int32_t(smp.scaleX);
    pc.srcOffset[1] = sy0 * int32_t(smp.scaleY);
    pc.dstOffset[0] = dstX; pc.dstOffset[1] = dstY;
    pc.extent[0] = sx1 - sx0; pc.extent[1] = sy1 - sy0;
    pc.srcScale[0] = int32_t(smp.scaleX); pc.srcScale[1] = int32_t(smp.scaleY);
    pc.sampleOffset[0] = smp.offsetX; pc.sampleOffset[1] = smp.offsetY;
    // Depth is never averaged (DeriveResolveSampling collapses an averaging
    // selector on depth to its first sample), so these are always the identity
    // here -- set anyway so the block that is pushed is fully initialised.
    pc.tapDelta[0] = 0; pc.tapDelta[1] = 0;
    pc.tapWeight = 0.25f;
    pc.scale = 1.0f;
    // Reused field: 1 = float24 (kD24FS8), 0 = unorm24 (kD24S8). It was pinned
    // at 1 for every depth copy, which is wrong for this frame -- the scene
    // depth is kD24FS8 but the shadow maps at EDRAM 0x5a0 are kD24S8.
    //
    // CHANGES NOTHING TODAY, and saying otherwise would be a false fix: the
    // shader computes both encodings and DISCARDS them (`(void)depth24` in
    // BuildDepthResolveComputeShader), writing the decoded float depth,
    // because the consumers fetch this destination as a depth texture and the
    // texture unit would decode it again. Verified: the shadow map's dump is
    // byte-identical before and after. What this fixes is a selector that was
    // silently wrong, so it cannot be believed if the encode is ever used.
    pc.swapRB = isFloat24 ? 1u : 0u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P.resolveDepthPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        P.resolveDepthLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, P.resolveDepthLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(pc), &pc);
    vkCmdDispatch(cmd, (uint32_t(pc.extent[0]) + 7) / 8,
                       (uint32_t(pc.extent[1]) + 7) / 8, 1);

    VkImageMemoryBarrier post[2]{};
    post[0] = pre[0];
    post[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    post[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    post[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    post[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    post[1] = pre[1];
    post[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    post[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, post);
    ++dst.copies;
    dst.everWritten = true;
}

void RenderTargetCache::ResolveSurfaceTo(VkCommandBuffer cmd, const SurfaceTarget& srcTarget,
                                         ResolveTarget& dst, const VkRect2D& srcRect,
                                         int32_t dstX, int32_t dstY, float scale,
                                         bool swapRB, const ResolveSampling& smp)
{
    // Clamp the rectangle to both images. A rectangle larger than either
    // cannot make the dispatch write out of bounds. In PIXELS -- see the note
    // in ResolveDepthTo: W and H are the sample grid.
    const int32_t pw = int32_t(W / std::max(1u, smp.scaleX));
    const int32_t ph = int32_t(H / std::max(1u, smp.scaleY));
    const int32_t sx0 = std::clamp<int32_t>(srcRect.offset.x, 0, pw);
    const int32_t sy0 = std::clamp<int32_t>(srcRect.offset.y, 0, ph);
    int32_t sx1 = std::clamp<int32_t>(srcRect.offset.x + int32_t(srcRect.extent.width),
                                      sx0, pw);
    int32_t sy1 = std::clamp<int32_t>(srcRect.offset.y + int32_t(srcRect.extent.height),
                                      sy0, ph);
    const int32_t dw = int32_t(dst.width), dh = int32_t(dst.imageHeight);
    if (dstX >= dw || dstY >= dh)
        return;
    sx1 = std::min<int32_t>(sx1, sx0 + std::max<int32_t>(0, dw - dstX));
    sy1 = std::min<int32_t>(sy1, sy0 + std::max<int32_t>(0, dh - dstY));
    if (sx1 <= sx0 || sy1 <= sy0)
        return;
    // The compute resolve is the only path that can apply the guest's
    // copy_dest_exp_bias and copy_dest_swap, so it is the default. It earned
    // that: forced to scale 1.0 with the swap suppressed -- where it must be
    // identical to the blit it replaces -- it reproduces it byte for byte
    // (0 of 2764816 differ). GEARS_DRAW_RESOLVE_BLIT=1 goes back to the
    // blit, as a control arm.
    static const bool blitResolve =
        lucent::config::flag("DRAW_RESOLVE_BLIT");
    const bool computeResolve = !blitResolve;
    const bool canCompute = computeResolve &&
        P.resolvePipeline != VK_NULL_HANDLE && !resolveSets.empty() &&
        srcTarget.storageView != VK_NULL_HANDLE &&
        dst.storageView != VK_NULL_HANDLE;
    if (!canCompute)
    {
        if (computeResolve)
            ++resolvesUnstorable;
        // The blit path: a rectangle copy to the destination offset,
        // preserving what is already there. It cannot scale or swap
        // channels, so the guest's exponent bias and red/blue swap are NOT
        // applied -- it is a control arm, not an equal alternative.
        VkImageMemoryBarrier tb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        tb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        tb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        tb.oldLayout = dst.everWritten ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_UNDEFINED;
        tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        tb.srcQueueFamilyIndex = tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tb.image = dst.image;
        tb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &tb);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[0] = {sx0, sy0, 0};
        bl.srcOffsets[1] = {sx1, sy1, 1};
        bl.dstOffsets[0] = {dstX, dstY, 0};
        bl.dstOffsets[1] = {dstX + (sx1 - sx0), dstY + (sy1 - sy0), 1};
        vkCmdBlitImage(cmd, srcTarget.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
            VK_FILTER_NEAREST);
        VkImageMemoryBarrier rb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        rb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        rb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        rb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        rb.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        rb.srcQueueFamilyIndex = rb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        rb.image = dst.image;
        rb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &rb);
        ++dst.copies;
        dst.everWritten = true;
        return;
    }

    // Both images into GENERAL, the only layout a storage image may be read
    // or written in. The source is left in TRANSFER_SRC_OPTIMAL by its
    // render pass; the destination keeps whatever it already holds, because
    // the other predicated tile's rows have to survive this dispatch.
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier toGeneral[2]{};
    toGeneral[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toGeneral[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toGeneral[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toGeneral[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGeneral[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral[0].srcQueueFamilyIndex = toGeneral[0].dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    toGeneral[0].image = srcTarget.color;
    toGeneral[0].subresourceRange = range;
    toGeneral[1] = toGeneral[0];
    toGeneral[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toGeneral[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    // PRESERVE: SHADER_READ_ONLY_OPTIMAL is where the previous resolve left
    // it, and it is the layout the sampling descriptors declare. UNDEFINED
    // only the first time, when there is nothing to preserve.
    toGeneral[1].oldLayout = dst.everWritten
        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral[1].image = dst.image;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, toGeneral);

    if (resolveSetsUsed >= resolveSets.size())
    {
        // Never wrap: reusing a set means an earlier dispatch reads the
        // descriptors a later one wrote, and that failure is invisible in
        // the frame -- it just makes some resolve targets empty.
        ++resolvesOutOfSets;
        return;
    }
    VkDescriptorSet set = resolveSets[resolveSetsUsed++];
    VkDescriptorImageInfo srcInfo{VK_NULL_HANDLE, srcTarget.storageView,
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, dst.storageView,
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[0].dstSet = set; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[0].pImageInfo = &srcInfo;
    w[1] = w[0];
    w[1].dstBinding = 1;
    w[1].pImageInfo = &dstInfo;
    vkUpdateDescriptorSets(R.device, 2, w, 0, nullptr);

    draw::ResolvePushConstants pc{};
    // srcOffset is in SAMPLES, extent in destination PIXELS -- the dispatch is
    // one invocation per destination pixel and the source steps srcScale.
    pc.srcOffset[0] = sx0 * int32_t(smp.scaleX);
    pc.srcOffset[1] = sy0 * int32_t(smp.scaleY);
    pc.dstOffset[0] = dstX; pc.dstOffset[1] = dstY;
    pc.extent[0] = sx1 - sx0; pc.extent[1] = sy1 - sy0;
    pc.srcScale[0] = int32_t(smp.scaleX); pc.srcScale[1] = int32_t(smp.scaleY);
    pc.sampleOffset[0] = smp.offsetX; pc.sampleOffset[1] = smp.offsetY;
    pc.tapDelta[0] = smp.spanX; pc.tapDelta[1] = smp.spanY;
    pc.tapWeight = 0.25f;
    pc.scale = scale;
    pc.swapRB = swapRB ? 1u : 0u;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P.resolvePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P.resolveLayout,
        0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, P.resolveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(pc), &pc);
    vkCmdDispatch(cmd, (uint32_t(pc.extent[0]) + 7) / 8,
                       (uint32_t(pc.extent[1]) + 7) / 8, 1);

    // The source goes back to TRANSFER_SRC_OPTIMAL, which is the layout its
    // render pass expects to resume from; the destination stays in GENERAL
    // and is made visible to the shaders that sample it.
    VkImageMemoryBarrier back[2]{};
    back[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    back[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    back[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back[0].srcQueueFamilyIndex = back[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back[0].image = srcTarget.color;
    back[0].subresourceRange = range;
    back[1] = back[0];
    back[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    back[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    back[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    back[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    back[1].image = dst.image;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 2, back);
    ++dst.copies;
    dst.everWritten = true;
}

} // namespace gears::draw
