// The render-target cache. gpu_draw_targets.h says what it is; this is how each
// lookup decides, and those decisions are why a frame assembles rather than
// overwriting itself.

#include "gpu_draw_targets.h"

#include <algorithm>
#include <cstring>

#include <lucent/config.h>
#include <lucent/log.h>

#include "gpu_draw_formats.h"
#include "gpu_draw_pixels.h"
#include "gpu_surface_format_capacity.h"
#include "gpu_draw_xlate.h"

namespace gears::draw
{

void ReleaseSurfaceTarget(VkDevice device, SurfaceTarget &surface)
{
    for (auto &[depthBase, framebuffer] : surface.fbs)
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    vkDestroyImageView(device, surface.resolvedStorageView, nullptr);
    vkDestroyImage(device, surface.resolvedColor, nullptr);
    vkFreeMemory(device, surface.resolvedColorMem, nullptr);
    vkDestroyImageView(device, surface.storageView, nullptr);
    vkDestroyImageView(device, surface.colorView, nullptr);
    vkDestroyImage(device, surface.color, nullptr);
    vkFreeMemory(device, surface.colorMem, nullptr);
    surface = {};
}

bool RenderTargetCache::MakeRenderPass(VkFormat colorFormat, VkSampleCountFlagBits samples,
                                       bool load, VkRenderPass &out)
{
    VkAttachmentDescription att[2]{};
    att[0].format = colorFormat;
    att[0].samples = samples;
    att[0].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = load ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = depthFormat;
    att[1].samples = samples;
    att[1].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // STENCIL IS CARRIED, not discarded. DONT_CARE here throws away the marks a
    // shadow pass just made, which is the same as having no stencil at all: the
    // pass that reads them runs a segment later, after the render pass has been
    // ended and re-begun for a resolve or a surface change (catalog #91).
    att[1].stencilLoadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].initialLayout =
        load ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(R.device, &rp, nullptr, &out));
    return true;
}

bool RenderTargetCache::GetPasses(VkFormat colorFormat, VkSampleCountFlagBits samples,
                                  std::pair<VkRenderPass, VkRenderPass> *&out)
{
    const auto key = std::make_pair(colorFormat, samples);
    auto it = P.passes.find(key);
    if (it == P.passes.end())
    {
        std::pair<VkRenderPass, VkRenderPass> rp{VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (!MakeRenderPass(colorFormat, samples, false, rp.first) ||
            !MakeRenderPass(colorFormat, samples, true, rp.second))
            return false;
        it = P.passes.emplace(key, rp).first;
    }
    out = &it->second;
    return true;
}

// The render-target cache proper: a host colour target per EDRAM surface,
// created the first time a frame renders into that (base, format) and kept
// for the life of the run like every other persistent object here. The image
// is promoted when a later frame needs a wider host container: the first boot
// frame only uses k_8_8_8_8 on base 0x2d0, while gameplay later reinterprets
// that same base as HDR and fixed-point formats. Returning the first image
// forever would force those later draws through an RGBA8 container.
bool RenderTargetCache::GetSurfaceTarget(uint32_t base, const DrawSampleLayout &layout,
                                         SurfaceTarget *&out)
{
    auto &targets = layout.IsNativeMultisample() ? P.surfaceTargets2x : P.surfaceTargets;
    auto fmts = formatsPerBase.find(base);
    if (fmts == formatsPerBase.end())
        return false;
    std::set<uint32_t> requiredFormats = fmts->second;
    auto it = targets.find(base);
    if (it != targets.end())
    {
        AccumulateSurfaceFormats(requiredFormats, it->second.guestFormats);
        bool mixed = false;
        const VkFormat requiredHostFormat = HostFormatFor(requiredFormats, mixed);
        if (requiredHostFormat == VK_FORMAT_UNDEFINED)
            return false;
        if (requiredHostFormat == it->second.hostFormat)
        {
            it->second.guestFormats = std::move(requiredFormats);
            out = &it->second;
            return true;
        }

        lucent::info("draw",
                     "render-target cache: promoting persistent surface {:#x} from host"
                     " format {} to {} because a later frame added guest format capacity",
                     base, int(it->second.hostFormat), int(requiredHostFormat));
        if (!R.frameSlots.WaitInFlight())
            return false;
        ReleaseSurfaceTarget(R.device, it->second);
        targets.erase(it);
    }
    bool mixed = false;
    const VkFormat hostFormat = HostFormatFor(requiredFormats, mixed);
    if (hostFormat == VK_FORMAT_UNDEFINED)
        return false;
    std::pair<VkRenderPass, VkRenderPass> *rp = nullptr;
    const auto samples = VkSampleCountFlagBits(layout.rasterSamples);
    if (!GetPasses(hostFormat, samples, rp))
        return false;
    SurfaceTarget s;
    s.guestFormats = std::move(requiredFormats);
    s.hostFormat = hostFormat;
    s.samples = samples;
    s.width = layout.imageWidth;
    s.height = layout.imageHeight;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = hostFormat;
    ci.extent = {s.width, s.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC so a resolve can copy it out and the presented surface
    // can be read back.
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // STORAGE: the resolve reads the surface in a compute pass. Only when
    // the host can store this format -- see FormatSupportsStorage.
    const bool formatCanStore = FormatSupportsStorage(R.physical, ci.format);
    const bool canStore = samples == VK_SAMPLE_COUNT_1_BIT && formatCanStore;
    if (canStore)
        ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (vkCreateImage(R.device, &ci, nullptr, &s.color) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(R.device, s.color, &req);
    uint32_t type = 0;
    if (!R.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
        R.FindMemory(req.memoryTypeBits, 0, type);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(R.device, &ai, nullptr, &s.colorMem) != VK_SUCCESS ||
        vkBindImageMemory(R.device, s.color, s.colorMem, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = s.color;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = hostFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &s.colorView) != VK_SUCCESS)
        return false;
    // The view the resolve dispatch READS through -- plain 2D, same image.
    if (canStore)
    {
        VkImageViewCreateInfo si = vi;
        si.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (vkCreateImageView(R.device, &si, nullptr, &s.storageView) != VK_SUCCESS)
            return false;
    }
    if (samples != VK_SAMPLE_COUNT_1_BIT)
    {
        // Vulkan resolves the two diagonal coverage samples into this ordinary
        // image first. The existing compute resolve remains the sole owner of
        // Xenos exponent bias and channel swapping, so this is representation
        // conversion rather than a second copy implementation.
        if (!formatCanStore)
        {
            lucent::error("draw",
                          "render-target cache: native {}X surface {:#x} uses host format {}"
                          " without storage-image support; its resolved values cannot be"
                          " passed through the Xenos colour-copy conversion",
                          layout.rasterSamples, base, int(hostFormat));
            return false;
        }
        VkImageCreateInfo ri = ci;
        ri.samples = VK_SAMPLE_COUNT_1_BIT;
        ri.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                   VK_IMAGE_USAGE_STORAGE_BIT;
        if (vkCreateImage(R.device, &ri, nullptr, &s.resolvedColor) != VK_SUCCESS)
            return false;
        VkMemoryRequirements resolvedReq{};
        vkGetImageMemoryRequirements(R.device, s.resolvedColor, &resolvedReq);
        uint32_t resolvedType = 0;
        if (!R.FindMemory(resolvedReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          resolvedType))
            R.FindMemory(resolvedReq.memoryTypeBits, 0, resolvedType);
        VkMemoryAllocateInfo resolvedAi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        resolvedAi.allocationSize = resolvedReq.size;
        resolvedAi.memoryTypeIndex = resolvedType;
        if (vkAllocateMemory(R.device, &resolvedAi, nullptr, &s.resolvedColorMem) != VK_SUCCESS ||
            vkBindImageMemory(R.device, s.resolvedColor, s.resolvedColorMem, 0) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo resolvedVi = vi;
        resolvedVi.image = s.resolvedColor;
        if (vkCreateImageView(R.device, &resolvedVi, nullptr, &s.resolvedStorageView) != VK_SUCCESS)
            return false;
    }
    // The framebuffers come later, one per DEPTH BASE this surface is drawn
    // with: a framebuffer names its attachments, and this surface is rendered
    // against more than one depth target in a frame.
    {
        lucent::Line nl;
        nl.add("render-target cache: new surface {:#x} ->", base);
        for (uint32_t f : fmts->second)
            nl.add(" {}", ColorFormatName(f));
        nl.add(" in one host target {}x{} at {} sample(s){}", s.width, s.height,
               layout.rasterSamples,
               mixed ? " (reinterpreted mid-frame; widened host format)" : "");
        nl.flush(lucent::Level::Info, "draw");
    }
    // A LOSSY PATH THAT HAS NEVER BEEN EXERCISED, SAYING SO INSTEAD OF
    // RENDERING QUIETLY WRONG. k_16_16 and k_16_16_16_16 hold -32..32 on the
    // console; their host formats here are SNORM, which holds -1..1. Xenia
    // handles this by subtracting 5 from the colour exponent bias so the
    // shader writes value/32 and the resolve scales it back; this renderer
    // does not, so anything outside -1..1 would truncate. No frame captured
    // from this title has ever taken the path -- every surface that uses
    // those formats also uses another, which widens the host format to float
    // and makes the question moot -- so it is UNTESTED rather than known
    // good, and a first occurrence must not look like a normal frame.
    if (s.hostFormat == VK_FORMAT_R16G16_SNORM || s.hostFormat == VK_FORMAT_R16G16B16A16_SNORM)
        lucent::warn("draw",
                     "surface {:#x} has an SNORM host format: the"
                     " guest's -32..32 range is TRUNCATED to -1..1 and this renderer"
                     " does not apply the /32 remap that would preserve it (Xenia does,"
                     " via color_exp_bias -= 5). Anything this surface renders outside"
                     " -1..1 is wrong. This path has never been exercised before now",
                     base);
    out = &targets.emplace(base, s).first->second;
    return true;
}

uint32_t resolveNoFormat = 0;
// Resolves whose rectangle could not be read from vf0.
uint32_t resolveNoRect = 0;
// Bumped when a NEW resolve target appears. A cached set names image views,
// and a view is a handle: a resolve re-running writes new CONTENTS through the
// same handle, which needs no invalidation. What does invalidate is a
// destination being seen for the first time mid-frame, because a draw before
// that point resolved the same binding to a stub or to guest memory.
uint64_t resolveGeneration = 0;

// ONE DEPTH+STENCIL IMAGE PER RB_DEPTH_INFO.depth_base, created on demand.
//
// The console addresses depth by an EDRAM base exactly as it addresses colour.
// This renderer held ONE image for the frame, and this title uses two bases:
// the scene renders against 0x0 and the shadow atlas against 0x5a0. The atlas
// passes therefore scribbled over the SCENE's stencil, and the shadow-mask pass
// that tests it was rejected everywhere they had drawn -- three full-screen mask
// draws rasterising and invoking no fragment shader, with the mask surviving
// exactly where neither 422x422 atlas tile had reached (catalog #91).
//
// Binding is a side effect on purpose: `P.depth` and the three views are the
// handles of the CURRENT target, so the mid-frame clear, the depth resolve, the
// aliasing pass and the probes all keep reading one place. A caller that asks
// for a base is asking to work on it.
// The framebuffer pairing this colour surface with one depth base, made on
// first use. A framebuffer names its attachments, so a surface rendered against
// two depth targets needs two of them -- and surface 0x2d0 of a gameplay frame
// is rendered against both the scene's depth and the shadow atlas's.
bool RenderTargetCache::GetFramebuffer(SurfaceTarget &s, uint32_t depthBase, VkFramebuffer &out)
{
    auto it = s.fbs.find(depthBase);
    if (it != s.fbs.end())
    {
        out = it->second;
        return true;
    }
    DepthTarget *d = nullptr;
    DrawSampleLayout layout{s.width, s.height, uint32_t(s.samples), 1, 1};
    if (!GetDepthTarget(depthBase, layout, d) || !d)
        return false;
    std::pair<VkRenderPass, VkRenderPass> *rp = nullptr;
    if (!GetPasses(s.hostFormat, s.samples, rp))
        return false;
    VkImageView atts[2] = {s.colorView, d->attachView};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = rp->first;
    fi.attachmentCount = 2;
    fi.pAttachments = atts;
    fi.width = s.width;
    fi.height = s.height;
    fi.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(R.device, &fi, nullptr, &fb) != VK_SUCCESS)
        return false;
    out = s.fbs.emplace(depthBase, fb).first->second;
    return true;
}

bool RenderTargetCache::GetDepthTarget(uint32_t base, const DrawSampleLayout &layout,
                                       DepthTarget *&out)
{
    auto bind = [&](DepthTarget &d)
    {
        P.depth = d.image;
        P.depthMem = d.mem;
        P.depthView = d.attachView;
        P.depthSampledView = d.depthSampledView;
        P.stencilSampledView = d.stencilSampledView;
        P.boundDepthBase = base;
        P.boundDepthSamples = d.samples;
        out = &d;
    };
    auto &targets = layout.IsNativeMultisample() ? P.depthTargets2x : P.depthTargets;
    auto it = targets.find(base);
    if (it != targets.end())
    {
        bind(it->second);
        return true;
    }

    DepthTarget d;
    d.samples = VkSampleCountFlagBits(layout.rasterSamples);
    d.width = layout.imageWidth;
    d.height = layout.imageHeight;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = depthFormat;
    ci.extent = {d.width, d.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = d.samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // SAMPLED: the depth resolve and the aliasing pass read it in compute, and a
    // depth image cannot be a storage image. TRANSFER_DST: the guest's own
    // mid-frame depth clear is a vkCmdClearDepthStencilImage, which requires
    // TRANSFER_DST. TRANSFER_SRC is the shipping depth/stencil probe's direct
    // readback path; omitting it made every GEARS_DRAW_DEPTH_DUMP recording
    // invalid even though the copied bytes could look plausible.
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(R.device, &ci, nullptr, &d.image) != VK_SUCCESS)
    {
        lucent::warn("draw",
                     "render-target cache: could not create the depth"
                     " target for base {:#x}; draws against it will use the last one"
                     " bound, which is the shared-depth behaviour this replaced",
                     base);
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(R.device, d.image, &req);
    uint32_t type = 0;
    if (!R.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
        R.FindMemory(req.memoryTypeBits, 0, type);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(R.device, &ai, nullptr, &d.mem) != VK_SUCCESS ||
        vkBindImageMemory(R.device, d.image, d.mem, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = d.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = depthFormat;
    // The ATTACHMENT view names both aspects; a sampled view may name only one,
    // which is why there are three.
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &d.attachView) != VK_SUCCESS)
        return false;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &d.depthSampledView) != VK_SUCCESS)
        return false;
    vi.subresourceRange = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &d.stencilSampledView) != VK_SUCCESS)
        return false;
    lucent::info("draw",
                 "render-target cache: new DEPTH target for base {:#x}"
                 " ({}x{}, {} sample(s)), {} in this view so far",
                 base, d.width, d.height, layout.rasterSamples, targets.size() + 1);
    bind(targets.emplace(base, d).first->second);
    return true;
}

bool RenderTargetCache::GetResolveTarget(uint32_t destBase, uint32_t sourceBase, uint32_t destPitch,
                                         uint32_t destHeight, uint32_t logicalWidth,
                                         uint32_t logicalHeight, uint32_t destFormat, bool isDepth,
                                         ResolveTarget *&out, uint32_t &rowOffsetOut)
{
    rowOffsetOut = 0;
    auto it = P.resolveTargets.find(destBase);
    if (it != P.resolveTargets.end())
    {
        out = &it->second;
        return true;
    }
    ++resolveGeneration; // a view that did not exist for earlier draws
    // Does this base fall INSIDE a texture we already have? That is the
    // predicated-tile case: the guest folds the tile's row offset into
    // RB_COPY_DEST_BASE, so the second tile's base is the first tile's base
    // plus whole rows. Matching on containment (same pitch, same format, a
    // whole number of rows in) assembles them into the one texture they are.
    const uint32_t bpp = ColorFormatBytesPerPixel(destFormat);
    if (bpp != 0 && destPitch != 0)
    {
        for (auto &[k, r] : P.resolveTargets)
        {
            if (r.pitch != destPitch || r.bpp != bpp || destBase <= r.base)
                continue;
            const uint64_t rowBytes = uint64_t(r.pitch) * r.bpp;
            const uint64_t delta = uint64_t(destBase) - r.base;
            if (delta % rowBytes != 0)
                continue;
            const uint64_t row = delta / rowBytes;
            if (row >= r.height)
                continue;
            rowOffsetOut = uint32_t(row);
            out = &r;
            lucent::debug("draw",
                          "render-target cache: resolve destination"
                          " {:#x} is row {} of the texture at {:#x} ({}x{}), not a"
                          " target of its own",
                          destBase, rowOffsetOut, r.base, r.pitch, r.height);
            return true;
        }
    }
    if (bpp == 0)
        ++resolveNoFormat;
    // A depth destination holds a float depth in .x, so it gets full
    // 32-bit precision; a colour one gets the wide float container.
    const VkFormat hostFormat = isDepth ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT;
    ResolveTarget r;
    r.isDepth = isDepth;
    r.hostFormat = hostFormat;
    r.sourceBase = sourceBase;
    r.base = destBase;
    r.pitch = destPitch;
    r.height = destHeight;
    r.bpp = bpp;
    r.guestFormat = destFormat;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = hostFormat;
    // RB_COPY_DEST_PITCH is the guest-memory ROW STRIDE, not the sampled
    // texture width. UE3's bloom target is logically 322x182 at pitch 352.
    // A Vulkan sampled image made 352 pixels wide changes every normalized
    // texture coordinate and makes the first blur lose the upper-right glow.
    // Use the unique consumer fetch extent when the frame supplies one; keep
    // pitch/height separately above for routing and pass identity.
    const uint32_t fallbackWidth = destPitch ? destPitch : W;
    const uint32_t fallbackHeight = destHeight ? destHeight : H;
    r.width = std::min<uint32_t>(logicalWidth ? logicalWidth : fallbackWidth, 8192);
    r.imageHeight = std::min<uint32_t>(logicalHeight ? logicalHeight : fallbackHeight, 8192);
    ci.extent = {r.width, r.imageHeight, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC is required by the per-resolve readback instrument. Keep it
    // on every destination because the image's usage is fixed at creation and
    // dumping is a runtime control, not a different cache type.
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const bool canStore = FormatSupportsStorage(R.physical, ci.format);
    if (canStore)
        ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT; // the resolve writes it
    if (vkCreateImage(R.device, &ci, nullptr, &r.image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(R.device, r.image, &req);
    uint32_t type = 0;
    if (!R.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
        R.FindMemory(req.memoryTypeBits, 0, type);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(R.device, &ai, nullptr, &r.mem) != VK_SUCCESS ||
        vkBindImageMemory(R.device, r.image, r.mem, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r.image;
    // 2D_ARRAY because the translated shaders declare their 2D textures as
    // arrays, exactly as the stub and guest texture views do.
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = hostFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(R.device, &vi, nullptr, &r.view) != VK_SUCCESS)
        return false;
    // The storage view the resolve dispatch writes through: same image,
    // plain 2D, because a storage image binding cannot be an array view.
    if (canStore)
    {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        if (vkCreateImageView(R.device, &vi, nullptr, &r.storageView) != VK_SUCCESS)
            return false;
    }
    lucent::info("draw",
                 "render-target cache: resolve destination {:#x} <- surface"
                 " {:#x} (guest pitch/height {}x{}, logical sampled extent {}x{},"
                 " {} bytes/px)",
                 destBase, sourceBase, destPitch, destHeight, r.width, r.imageHeight, bpp);
    out = &P.resolveTargets.emplace(destBase, r).first->second;
    return true;
}

void RenderTargetCache::BuildResolvePipeline()
{
    // --- the resolve compute pipeline -----------------------------------
    // Built once. It applies the guest's copy_dest_exp_bias and copy_dest_swap
    // while copying the rectangle, which is why a resolve is a dispatch and not
    // a vkCmdBlitImage (catalog #33).
    if (P.resolvePipeline == VK_NULL_HANDLE && R.hasStorageImageWithoutFormat)
    {
        std::vector<uint32_t> spirv;
        if (BuildResolveComputeShader(spirv))
        {
            VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            smi.codeSize = spirv.size() * sizeof(uint32_t);
            smi.pCode = spirv.data();
            const VkDescriptorSetLayoutBinding binds[2] = {
                {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
            VkDescriptorSetLayoutCreateInfo sli{
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            sli.bindingCount = 2;
            sli.pBindings = binds;
            VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ResolvePushConstants)};
            if (vkCreateShaderModule(R.device, &smi, nullptr, &P.resolveModule) == VK_SUCCESS &&
                vkCreateDescriptorSetLayout(R.device, &sli, nullptr, &P.resolveSetLayout) ==
                    VK_SUCCESS)
            {
                VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                pli.setLayoutCount = 1;
                pli.pSetLayouts = &P.resolveSetLayout;
                pli.pushConstantRangeCount = 1;
                pli.pPushConstantRanges = &pcr;
                if (vkCreatePipelineLayout(R.device, &pli, nullptr, &P.resolveLayout) == VK_SUCCESS)
                {
                    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                    cpi.stage.module = P.resolveModule;
                    cpi.stage.pName = "main";
                    cpi.layout = P.resolveLayout;
                    if (vkCreateComputePipelines(R.device, VK_NULL_HANDLE, 1, &cpi, nullptr,
                                                 &P.resolvePipeline) != VK_SUCCESS)
                        P.resolvePipeline = VK_NULL_HANDLE;
                }
            }
        }
        // The DEPTH resolve pipeline: one sampled image in, one storage image
        // out. A depth image cannot be a storage image, so its descriptor type
        // differs from the colour resolve's and it needs its own layout.
        if (P.resolveDepthPipeline == VK_NULL_HANDLE)
        {
            std::vector<uint32_t> dspirv;
            if (BuildDepthResolveComputeShader(dspirv))
            {
                VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                smi.codeSize = dspirv.size() * sizeof(uint32_t);
                smi.pCode = dspirv.data();
                const VkDescriptorSetLayoutBinding dbinds[2] = {
                    {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
                VkDescriptorSetLayoutCreateInfo dsli{
                    VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
                dsli.bindingCount = 2;
                dsli.pBindings = dbinds;
                VkPushConstantRange dpcr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                         sizeof(ResolvePushConstants)};
                if (vkCreateShaderModule(R.device, &smi, nullptr, &P.resolveDepthModule) ==
                        VK_SUCCESS &&
                    vkCreateDescriptorSetLayout(R.device, &dsli, nullptr,
                                                &P.resolveDepthSetLayout) == VK_SUCCESS)
                {
                    VkPipelineLayoutCreateInfo dpli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                    dpli.setLayoutCount = 1;
                    dpli.pSetLayouts = &P.resolveDepthSetLayout;
                    dpli.pushConstantRangeCount = 1;
                    dpli.pPushConstantRanges = &dpcr;
                    if (vkCreatePipelineLayout(R.device, &dpli, nullptr, &P.resolveDepthLayout) ==
                        VK_SUCCESS)
                    {
                        VkComputePipelineCreateInfo dcpi{
                            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                        dcpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                        dcpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                        dcpi.stage.module = P.resolveDepthModule;
                        dcpi.stage.pName = "main";
                        dcpi.layout = P.resolveDepthLayout;
                        if (vkCreateComputePipelines(R.device, VK_NULL_HANDLE, 1, &dcpi, nullptr,
                                                     &P.resolveDepthPipeline) != VK_SUCCESS)
                            P.resolveDepthPipeline = VK_NULL_HANDLE;
                    }
                }
            }
            if (P.resolveDepthPipeline != VK_NULL_HANDLE)
                lucent::info("draw", "depth resolve compute pipeline built");
            else
                lucent::error("draw", "depth resolve compute pipeline unavailable --"
                                      " passes that sample resolved depth will keep reading stale"
                                      " guest memory");

            // Same descriptor and push-constant ABI, but the source image's
            // SPIR-V type is multisampled and OpImageFetch takes a sample
            // operand. Vulkan requires that distinction in the shader type;
            // binding the 2X image to the single-sample module is invalid.
            if (P.resolveDepthLayout != VK_NULL_HANDLE &&
                P.resolveDepth2xPipeline == VK_NULL_HANDLE)
            {
                std::vector<uint32_t> d2xSpirv;
                if (BuildDepthResolveComputeShader(d2xSpirv, true))
                {
                    VkShaderModuleCreateInfo d2xSmi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                    d2xSmi.codeSize = d2xSpirv.size() * sizeof(uint32_t);
                    d2xSmi.pCode = d2xSpirv.data();
                    if (vkCreateShaderModule(R.device, &d2xSmi, nullptr, &P.resolveDepth2xModule) ==
                        VK_SUCCESS)
                    {
                        VkComputePipelineCreateInfo d2xCpi{
                            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                        d2xCpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                        d2xCpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                        d2xCpi.stage.module = P.resolveDepth2xModule;
                        d2xCpi.stage.pName = "main";
                        d2xCpi.layout = P.resolveDepthLayout;
                        if (vkCreateComputePipelines(R.device, VK_NULL_HANDLE, 1, &d2xCpi, nullptr,
                                                     &P.resolveDepth2xPipeline) != VK_SUCCESS)
                            P.resolveDepth2xPipeline = VK_NULL_HANDLE;
                    }
                }
                if (P.resolveDepth2xPipeline != VK_NULL_HANDLE)
                    lucent::info("draw", "2X depth resolve compute pipeline built");
                else
                    lucent::error("draw", "2X depth resolve compute pipeline unavailable");
            }
        }

        if (P.resolvePipeline == VK_NULL_HANDLE)
            lucent::error("draw", "resolve compute pipeline unavailable -- resolves"
                                  " will copy without the guest's exponent bias or red/blue swap");
        else
            lucent::info("draw", "resolve compute pipeline built");
    }

    // Resolves that could not be attributed to a destination texture, counted
    // rather than silently dropped.
}

} // namespace gears::draw
