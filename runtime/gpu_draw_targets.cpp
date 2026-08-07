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
#include "gpu_draw_xlate.h"

namespace gears::draw
{

bool RenderTargetCache::MakeRenderPass(VkFormat colorFormat, bool load, VkRenderPass& out)
{
    VkAttachmentDescription att[2]{};
    att[0].format = colorFormat;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = load ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                : VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = depthFormat;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = load ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                : VK_IMAGE_LAYOUT_UNDEFINED;
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

bool RenderTargetCache::GetPasses(VkFormat colorFormat,
                                  std::pair<VkRenderPass, VkRenderPass>*& out)
{
    auto it = P.passes.find(colorFormat);
    if (it == P.passes.end())
    {
        std::pair<VkRenderPass, VkRenderPass> rp{VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (!MakeRenderPass(colorFormat, false, rp.first) ||
            !MakeRenderPass(colorFormat, true, rp.second))
            return false;
        it = P.passes.emplace(colorFormat, rp).first;
    }
    out = &it->second;
    return true;
}

std::map<uint32_t, std::set<uint32_t>> formatsPerBase;

// The render-target cache proper: a host colour target per EDRAM surface,
// created the first time a frame renders into that (base, format) and kept
// for the life of the run like every other persistent object here.
bool RenderTargetCache::GetSurfaceTarget(uint32_t base, SurfaceTarget*& out)
{
    auto it = P.surfaceTargets.find(base);
    if (it != P.surfaceTargets.end()) { out = &it->second; return true; }
    auto fmts = formatsPerBase.find(base);
    if (fmts == formatsPerBase.end())
        return false;
    bool mixed = false;
    const VkFormat hostFormat = HostFormatFor(fmts->second, mixed);
    if (hostFormat == VK_FORMAT_UNDEFINED)
        return false;
    std::pair<VkRenderPass, VkRenderPass>* rp = nullptr;
    if (!GetPasses(hostFormat, rp))
        return false;
    SurfaceTarget s;
    s.hostFormat = hostFormat;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = hostFormat;
    ci.extent = {W, H, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC so a resolve can copy it out and the presented surface
    // can be read back.
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // STORAGE: the resolve reads the surface in a compute pass. Only when
    // the host can store this format -- see FormatSupportsStorage.
    const bool canStore = FormatSupportsStorage(R.physical, ci.format);
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
    VkImageView atts[2] = {s.colorView, depthView};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = rp->first;
    fi.attachmentCount = 2;
    fi.pAttachments = atts;
    fi.width = W;
    fi.height = H;
    fi.layers = 1;
    if (vkCreateFramebuffer(R.device, &fi, nullptr, &s.fb) != VK_SUCCESS)
        return false;
    {
        lucent::Line nl;
        nl.add("render-target cache: new surface {:#x} ->", base);
        for (uint32_t f : fmts->second)
            nl.add(" {}", ColorFormatName(f));
        nl.add(" in one host target {}x{}{}", W, H,
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
    if (s.hostFormat == VK_FORMAT_R16G16_SNORM ||
        s.hostFormat == VK_FORMAT_R16G16B16A16_SNORM)
        lucent::warn("draw", "surface {:#x} has an SNORM host format: the"
            " guest's -32..32 range is TRUNCATED to -1..1 and this renderer"
            " does not apply the /32 remap that would preserve it (Xenia does,"
            " via color_exp_bias -= 5). Anything this surface renders outside"
            " -1..1 is wrong. This path has never been exercised before now",
            base);
    out = &P.surfaceTargets.emplace(base, s).first->second;
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

bool RenderTargetCache::GetResolveTarget(uint32_t destBase, uint32_t sourceBase,
                                         uint32_t destPitch, uint32_t destHeight,
                                         uint32_t destFormat, bool isDepth,
                                         ResolveTarget*& out,
                                         uint32_t& rowOffsetOut)
{
    rowOffsetOut = 0;
    auto it = P.resolveTargets.find(destBase);
    if (it != P.resolveTargets.end()) { out = &it->second; return true; }
    ++resolveGeneration; // a view that did not exist for earlier draws
    // Does this base fall INSIDE a texture we already have? That is the
    // predicated-tile case: the guest folds the tile's row offset into
    // RB_COPY_DEST_BASE, so the second tile's base is the first tile's base
    // plus whole rows. Matching on containment (same pitch, same format, a
    // whole number of rows in) assembles them into the one texture they are.
    const uint32_t bpp = ColorFormatBytesPerPixel(destFormat);
    if (bpp != 0 && destPitch != 0)
    {
        for (auto& [k, r] : P.resolveTargets)
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
            lucent::info("draw", "render-target cache: resolve destination"
                " {:#x} is row {} of the texture at {:#x} ({}x{}), not a"
                " target of its own", destBase, rowOffsetOut, r.base,
                r.pitch, r.height);
            return true;
        }
    }
    if (bpp == 0)
        ++resolveNoFormat;
    // A depth destination holds a float depth in .x, so it gets full
    // 32-bit precision; a colour one gets the wide float container.
    const VkFormat hostFormat = isDepth ? VK_FORMAT_R32_SFLOAT
                                        : VK_FORMAT_R16G16B16A16_SFLOAT;
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
    // The texture's OWN size, as the guest declared it -- not the frame's.
    // A tile resolve declares the full destination height (720) even though
    // it writes only its own rows, which is exactly what makes the whole
    // texture addressable for the tile that follows.
    r.width = destPitch ? std::min<uint32_t>(destPitch, 8192) : W;
    r.imageHeight = destHeight ? std::min<uint32_t>(destHeight, 8192) : H;
    ci.extent = {r.width, r.imageHeight, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
    lucent::info("draw", "render-target cache: resolve destination {:#x} <- surface"
        " {:#x} ({}x{} px, {} bytes/px, host image {}x{})", destBase, sourceBase,
        destPitch, destHeight, bpp, r.width, r.imageHeight);
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
            VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    sizeof(ResolvePushConstants)};
            if (vkCreateShaderModule(R.device, &smi, nullptr, &P.resolveModule) == VK_SUCCESS &&
                vkCreateDescriptorSetLayout(R.device, &sli, nullptr, &P.resolveSetLayout) == VK_SUCCESS)
            {
                VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                pli.setLayoutCount = 1;
                pli.pSetLayouts = &P.resolveSetLayout;
                pli.pushConstantRangeCount = 1;
                pli.pPushConstantRanges = &pcr;
                if (vkCreatePipelineLayout(R.device, &pli, nullptr, &P.resolveLayout) == VK_SUCCESS)
                {
                    VkComputePipelineCreateInfo cpi{
                        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                    cpi.stage.module = P.resolveModule;
                    cpi.stage.pName = "main";
                    cpi.layout = P.resolveLayout;
                    if (vkCreateComputePipelines(R.device, VK_NULL_HANDLE, 1, &cpi,
                            nullptr, &P.resolvePipeline) != VK_SUCCESS)
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
                if (vkCreateShaderModule(R.device, &smi, nullptr, &P.resolveDepthModule) == VK_SUCCESS &&
                    vkCreateDescriptorSetLayout(R.device, &dsli, nullptr, &P.resolveDepthSetLayout) == VK_SUCCESS)
                {
                    VkPipelineLayoutCreateInfo dpli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                    dpli.setLayoutCount = 1;
                    dpli.pSetLayouts = &P.resolveDepthSetLayout;
                    dpli.pushConstantRangeCount = 1;
                    dpli.pPushConstantRanges = &dpcr;
                    if (vkCreatePipelineLayout(R.device, &dpli, nullptr, &P.resolveDepthLayout) == VK_SUCCESS)
                    {
                        VkComputePipelineCreateInfo dcpi{
                            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                        dcpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
                        dcpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                        dcpi.stage.module = P.resolveDepthModule;
                        dcpi.stage.pName = "main";
                        dcpi.layout = P.resolveDepthLayout;
                        if (vkCreateComputePipelines(R.device, VK_NULL_HANDLE, 1, &dcpi,
                                nullptr, &P.resolveDepthPipeline) != VK_SUCCESS)
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
