// EDRAM format reinterpretation: the pass that runs when a frame re-declares
// one EDRAM base under a different colour format.
//
// runtime/shaders/edram_reinterpret.comp says what the conversion IS and where
// each format's bit layout comes from. This file is the plumbing: one compute
// pipeline, one descriptor set per transfer, and the refusal path for a format
// pair the container cannot carry.
//
// WHY IT EXISTS. gpu_draw_renderer.h's render-target cache keys a host image on
// the EDRAM base alone and gives it a container wide enough for every format
// the frame declares there, on the reasoning that "a reinterpretation
// accumulates the way EDRAM does". It does not. EDRAM accumulates BITS; a
// widened float container accumulates VALUES, and the two only agree while the
// format does not change. Measured on walk_gameplay.gfr, guest draw 649 writes
// surface 0x2d0 as k_2_10_10_10 -- a fixed-point target, so the hardware clamps
// its (37.09, 30.98, 15.91) to 1.0 and stores the bits 0x3FF per channel -- and
// guest draw 650 declares k_2_10_10_10_FLOAT_AS_16_16_16_16 on the same base
// and BLENDS against it. The console reads 0x3FF back as the 7e3 float 31.875;
// this renderer read back the value 1.0 it had stored.
//
// The pass is off by default until it is verified against a frame:
// GEARS_DRAW_REINTERP=1 enables it, GEARS_DRAW_REINTERP_SELFTEST=1 proves the
// shader on this GPU rather than on paper.

#include "gpu_draw_targets.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <lucent/config.h>
#include <lucent/log.h>

#include "edram_reinterpret_spv.h"
#include "edram_depth_alias_spv.h"
#include "edram_depth_alias_spv.h"
#include "gpu_draw_formats.h"
#include "gpu_draw_pixels.h"

namespace gears::draw
{

// The formats this pass can convert. All 32 bits per pixel, and all with a
// value<->bit mapping the host container round-trips: an 8-bit or 10-bit UNORM
// level and a 7e3 float are each exactly recoverable from a float16 or wider
// container.
//
// Deliberately NOT here: k_16_16 and k_16_16_16_16 (fixed point -32..32, stored
// in this renderer as SNORM [-1,1] with the scale convention unverified), the
// 64bpp formats, and the 32-bit float ones. A frame that reinterprets through
// one of those is REPORTED, not approximated -- guessing the scale would make a
// wrong picture that looks like a converted one.
bool ReinterpretSupportedFormat(uint32_t storageFormat)
{
    return storageFormat == 0 || storageFormat == 1 || storageFormat == 2 ||
           storageFormat == 3;
}

bool RenderTargetCache::BuildReinterpretPipeline()
{
    if (P.reinterpretPipeline != VK_NULL_HANDLE)
        return true;
    if (!R.hasStorageImageWithoutFormat)
    {
        lucent::error("draw", "EDRAM reinterpretation unavailable: this device"
            " has no shaderStorageImageReadWithoutFormat, so a surface's host"
            " image cannot be read as a storage image of unknown format --"
            " every format change will be left unconverted");
        return false;
    }

    const std::vector<uint32_t>& spirv = gears::native::EdramReinterpretSpirv();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spirv.size() * sizeof(uint32_t);
    smi.pCode = spirv.data();
    if (vkCreateShaderModule(R.device, &smi, nullptr, &P.reinterpretModule) != VK_SUCCESS)
        return false;

    const VkDescriptorSetLayoutBinding bind{
        0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 1;
    sli.pBindings = &bind;
    if (vkCreateDescriptorSetLayout(R.device, &sli, nullptr,
                                    &P.reinterpretSetLayout) != VK_SUCCESS)
        return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof(ReinterpretPushConstants)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &P.reinterpretSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(R.device, &pli, nullptr, &P.reinterpretLayout) != VK_SUCCESS)
        return false;

    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = P.reinterpretModule;
    cpi.stage.pName = "main";
    cpi.layout = P.reinterpretLayout;
    if (vkCreateComputePipelines(R.device, VK_NULL_HANDLE, 1, &cpi, nullptr,
                                 &P.reinterpretPipeline) != VK_SUCCESS)
    {
        P.reinterpretPipeline = VK_NULL_HANDLE;
        return false;
    }
    lucent::info("draw", "EDRAM reinterpretation compute pipeline built");
    return true;
}

// EDRAM COLOUR/DEPTH ALIASING. The shader says what this IS and why the title
// needs it; this is the plumbing. Built lazily, like the reinterpretation
// pipeline, and reported once either way -- a frame that aliases with the pass
// unavailable must not look like a frame that does not alias.
bool RenderTargetCache::BuildDepthAliasPipeline()
{
    if (P.depthAliasPipeline != VK_NULL_HANDLE)
        return true;
    const std::vector<uint32_t>& spirv = gears::native::EdramDepthAliasSpirv();
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = spirv.size() * sizeof(uint32_t);
    smi.pCode = spirv.data();
    if (vkCreateShaderModule(R.device, &smi, nullptr, &P.depthAliasModule) != VK_SUCCESS)
        return false;
    // A sampler is required for the two sampled bindings even though the shader
    // only ever texelFetches: the binding type is COMBINED_IMAGE_SAMPLER.
    VkSamplerCreateInfo sai{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sai.magFilter = sai.minFilter = VK_FILTER_NEAREST;
    sai.addressModeU = sai.addressModeV = sai.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(R.device, &sai, nullptr, &P.depthAliasSampler) != VK_SUCCESS)
        return false;
    const VkDescriptorSetLayoutBinding binds[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 3;
    sli.pBindings = binds;
    if (vkCreateDescriptorSetLayout(R.device, &sli, nullptr,
                                    &P.depthAliasSetLayout) != VK_SUCCESS)
        return false;
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            sizeof(DepthAliasPushConstants)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &P.depthAliasSetLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(R.device, &pli, nullptr, &P.depthAliasLayout) != VK_SUCCESS)
        return false;
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = P.depthAliasModule;
    cpi.stage.pName = "main";
    cpi.layout = P.depthAliasLayout;
    if (vkCreateComputePipelines(R.device, VK_NULL_HANDLE, 1, &cpi, nullptr,
                                 &P.depthAliasPipeline) != VK_SUCCESS)
    {
        P.depthAliasPipeline = VK_NULL_HANDLE;
        return false;
    }
    lucent::info("draw", "EDRAM depth-alias compute pipeline built");
    return true;
}

bool RenderTargetCache::AliasDepthIntoSurface(VkCommandBuffer cmd, SurfaceTarget& t,
                                              bool isFloat24)
{
    if (P.depthAliasPipeline == VK_NULL_HANDLE || t.storageView == VK_NULL_HANDLE)
        return false;
    if (depthAliasSetsUsed >= depthAliasSets.size())
    {
        ++depthAliasOutOfSets;
        return false;
    }
    // The surface's own storage format decides what the depth bits READ AS, so
    // a surface whose format this shader does not unpack is refused rather than
    // written with a value from a format it does not implement.
    const uint32_t fmt = t.storageFormat;
    if (fmt != 0 && fmt != 1 && fmt != 2 && fmt != 3)
    {
        ++depthAliasRefused;
        depthAliasRefusedFormats.insert(fmt);
        return false;
    }

    VkImageMemoryBarrier pre[2]{};
    pre[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    pre[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    pre[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pre[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    pre[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    pre[0].srcQueueFamilyIndex = pre[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[0].image = P.depth;
    pre[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT |
                               VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
    pre[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    pre[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    pre[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pre[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    pre[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    pre[1].srcQueueFamilyIndex = pre[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre[1].image = t.color;
    pre[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre);

    VkDescriptorSet set = depthAliasSets[depthAliasSetsUsed++];
    VkDescriptorImageInfo d{P.depthAliasSampler, P.depthSampledView,
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo st{P.depthAliasSampler, P.stencilSampledView,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo c{VK_NULL_HANDLE, t.storageView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3]{};
    for (int i = 0; i < 3; ++i)
    {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = uint32_t(i);
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &d;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &st;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;          w[2].pImageInfo = &c;
    vkUpdateDescriptorSets(R.device, 3, w, 0, nullptr);

    DepthAliasPushConstants pc{};
    pc.extent[0] = int32_t(W);
    pc.extent[1] = int32_t(H);
    pc.isFloat24 = isFloat24 ? 1u : 0u;
    pc.dstFormat = fmt;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P.depthAliasPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        P.depthAliasLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, P.depthAliasLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(pc), &pc);
    vkCmdDispatch(cmd, (W + 7) / 8, (H + 7) / 8, 1);

    VkImageMemoryBarrier post[2]{};
    post[0] = pre[0];
    post[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    post[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    post[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    post[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    post[1] = pre[1];
    post[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    post[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 2, post);
    ++depthAliasesDone;
    return true;
}

bool RenderTargetCache::ReinterpretSurface(VkCommandBuffer cmd, SurfaceTarget& t,
                                           uint32_t fromFormat, uint32_t toFormat)
{
    const uint32_t from = StorageColorFormat(fromFormat);
    const uint32_t to = StorageColorFormat(toFormat);
    if (from == to)
        return true; // k_2_10_10_10 and its _AS_ variant store the same bits
    // GEARS_DRAW_NOCONVERT=<from>-<to>[,<from>-<to>...]: suppress ONE format
    // pair, in STORAGE form (0 k_8_8_8_8, 2 k_2_10_10_10, 3 k_2_10_10_10_FLOAT).
    //
    // The whole-pass switch (GEARS_DRAW_NOREINTERP) cannot answer "which
    // conversion is wrong": with it off the frame changes for six reasons at
    // once, and "the pass off is closer to the console" and "one of its six
    // conversions is wrong" produce the same single number. This is the arm
    // that separates them, and it reports what it suppressed so a run that
    // matched nothing cannot read as a run that changed nothing.
    // BY VALUE -- a reference into lucent's config cache dangles when the cache
    // is dropped (see gpu_draw_shaders.cpp).
    static const std::string suppress{lucent::config::text("DRAW_NOCONVERT")};
    if (!suppress.empty())
    {
        const std::string pair = std::to_string(from) + "-" + std::to_string(to);
        size_t at = 0;
        while (at < suppress.size())
        {
            size_t comma = suppress.find(',', at);
            if (comma == std::string::npos) comma = suppress.size();
            if (suppress.compare(at, comma - at, pair) == 0)
            {
                ++reinterpretsSuppressed;
                reinterpretSuppressedPairs.insert((uint64_t(from) << 32) | to);
                // NOT converted and NOT relabelled: the surface keeps the bits
                // and the format it had, which is exactly what this arm is for.
                return false;
            }
            at = comma + 1;
        }
    }
    if (!ReinterpretSupportedFormat(from) || !ReinterpretSupportedFormat(to))
    {
        ++reinterpretsRefused;
        reinterpretRefusedPairs.insert((uint64_t(from) << 32) | to);
        return false;
    }
    if (P.reinterpretPipeline == VK_NULL_HANDLE ||
        reinterpretSetsUsed >= reinterpretSets.size())
    {
        ++reinterpretsOutOfSets;
        return false;
    }

    // The colour attachment sits in TRANSFER_SRC_OPTIMAL between passes (see
    // MakeRenderPass: that is both the initial and the final layout of the LOAD
    // pass), and a storage image must be GENERAL.
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = t.color;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &bar);

    VkDescriptorSet set = reinterpretSets[reinterpretSetsUsed++];
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, t.storageView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(R.device, 1, &w, 0, nullptr);

    ReinterpretPushConstants pc{};
    pc.extent[0] = int32_t(W);
    pc.extent[1] = int32_t(H);
    pc.srcFormat = from;
    pc.dstFormat = to;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P.reinterpretPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            P.reinterpretLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, P.reinterpretLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(pc), &pc);
    vkCmdDispatch(cmd, (W + 7) / 8, (H + 7) / 8, 1);

    // Back to the layout the LOAD pass expects, and visible to it.
    VkImageMemoryBarrier post = bar;
    post.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    post.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    post.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &post);

    ++reinterpretsDone;
    reinterpretPairs.insert((uint64_t(from) << 32) | to);
    return true;
}

// --- the self-test ------------------------------------------------------
//
// GEARS_DRAW_REINTERP_SELFTEST=1 runs the SHIPPING module on this GPU against
// cases whose answers are arithmetic, before any frame is rendered. Every other
// way of checking this pass is circular: the frame it changes has no reference,
// and a shader that converted NOTHING would leave a picture that still looks
// like a picture.
//
// Five cases. The third is a format pair that must leave the values ALONE: a
// shader that does nothing passes that one and fails the first two, so the test
// tells "converts correctly" from "never ran" in both directions. The last two
// are a ROUND TRIP -- 7e3 to 8888 and back, the second starting from the first's
// measured output -- which is the property that makes converting the whole
// surface sound rather than destructive.
namespace
{
struct SelfTestCase
{
    const char* name;
    // The input is written as float16 BIT PATTERNS and decoded with the
    // renderer's own HalfToFloat before use, so a wrong constant here is caught
    // as a wrong input rather than read as a wrong shader.
    uint16_t in[4];
    float inValue[4];
    uint32_t from, to;
    float expect[4];
    // A chained case starts from the PREVIOUS case's measured output instead of
    // uploading anything: `in`/`inValue` are then unused. This is how the round
    // trip is tested without hand-writing float16 constants for an intermediate
    // value that float16 may not hold exactly -- the test would then be
    // measuring the constant rather than the shader.
    bool chain;
};
constexpr uint32_t kSelfTestCases = 5;
const SelfTestCase kCases[kSelfTestCases] = {
    // 1.0 stored under k_2_10_10_10 is the bits 0x3FF per channel, which
    // k_2_10_10_10_FLOAT reads back as the largest 7e3 value, 31.875. This is
    // the case measured on walk_gameplay.gfr across guest draws 649 -> 650.
    {"k_2_10_10_10 -> k_2_10_10_10_FLOAT (white)",
     {0x3C00, 0x3C00, 0x3C00, 0x3C00}, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 3,
     {31.875f, 31.875f, 31.875f, 1.0f}, false},
    // 0.5 under k_8_8_8_8 is 0x80 per channel -- the dword 0x80808080, whose
    // 10-bit fields are 0x080, 0x020 and 0x008 and whose 2-bit alpha field is
    // 2. Only the first is a NORMAL 7e3 number; the other two are denormals,
    // which is the half of Float7e3To32 an all-normal case never reaches.
    {"k_8_8_8_8 -> k_2_10_10_10_FLOAT (denormal fields)",
     {0x3800, 0x3800, 0x3800, 0x3800}, {0.5f, 0.5f, 0.5f, 0.5f}, 0, 3,
     {0.25f, 0.0625f, 0.015625f, 2.0f / 3.0f}, false},
    // THE NEGATIVE. White under k_8_8_8_8 is 0xFFFFFFFF, and every field of
    // k_2_10_10_10 reads full-scale from it, so this pair must change nothing.
    {"k_8_8_8_8 -> k_2_10_10_10 (white: must NOT change)",
     {0x3C00, 0x3C00, 0x3C00, 0x3C00}, {1.0f, 1.0f, 1.0f, 1.0f}, 0, 2,
     {1.0f, 1.0f, 1.0f, 1.0f}, false},
    // THE ROUND TRIP, in two halves. Converting the WHOLE surface at every
    // format change is only sound if the conversions preserve the console's
    // BITS -- a frame that declares 7e3, then 8888, then 7e3 again with nothing
    // written in between must end where it started, because on the console
    // nothing happened at all. The HDR value 3.0 is a 7e3 normal whose bits
    // (0x240 in the red field) read as 8888 give (64/255, 2/255, 0, 0), and
    // those must convert straight back.
    {"k_2_10_10_10_FLOAT -> k_8_8_8_8 (HDR 3.0, half a round trip)",
     {0x4200, 0x0000, 0x0000, 0x0000}, {3.0f, 0.0f, 0.0f, 0.0f}, 3, 0,
     {64.0f / 255.0f, 2.0f / 255.0f, 0.0f, 0.0f}, false},
    {"k_8_8_8_8 -> k_2_10_10_10_FLOAT (the other half: back to 3.0)",
     {0, 0, 0, 0}, {0, 0, 0, 0}, 0, 3, {3.0f, 0.0f, 0.0f, 0.0f}, true},
};
} // namespace

bool RenderTargetCache::ReinterpretSelfTest()
{
    if (!BuildReinterpretPipeline())
    {
        lucent::error("draw", "EDRAM reinterpretation self-test: the pipeline"
            " could not be built, so NOTHING was tested");
        return false;
    }

    // Each case gets its own 1x1 image, uploaded, dispatched and read back on
    // its own. Sharing a row would mean one dispatch's format pair covering
    // every texel in the group, which is exactly what would make the negative
    // case meaningless.
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory imgMem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory bufMem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    bool ok = true;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    // The host format the frame's mixed surfaces actually use, so the test
    // carries the container's precision and not a wider one's.
    ci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ci.extent = {1, 1, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(R.device, &ci, nullptr, &img) != VK_SUCCESS)
        ok = false;
    if (ok)
    {
        VkMemoryRequirements req{};
        uint32_t type = 0;
        vkGetImageMemoryRequirements(R.device, img, &req);
        if (!R.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
            R.FindMemory(req.memoryTypeBits, 0, type);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        ok = vkAllocateMemory(R.device, &ai, nullptr, &imgMem) == VK_SUCCESS &&
             vkBindImageMemory(R.device, img, imgMem, 0) == VK_SUCCESS;
    }
    if (ok)
    {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok = vkCreateImageView(R.device, &vi, nullptr, &view) == VK_SUCCESS;
    }
    if (ok)
        ok = R.MakeBuffer(8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          buf, bufMem, true);
    if (ok)
    {
        // A command pool of its own: the frame's command buffer is not open
        // yet, and this must not disturb it.
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = R.queueFamily;
        ok = vkCreateCommandPool(R.device, &pci, nullptr, &pool) == VK_SUCCESS;
    }
    if (ok)
    {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        ok = vkAllocateCommandBuffers(R.device, &cai, &cmd) == VK_SUCCESS;
    }
    if (ok)
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        ok = vkCreateDescriptorPool(R.device, &pi, nullptr, &descPool) == VK_SUCCESS;
    }
    if (ok)
    {
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = descPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &P.reinterpretSetLayout;
        ok = vkAllocateDescriptorSets(R.device, &dai, &set) == VK_SUCCESS;
    }

    auto destroy = [&]() {
        vkDestroyDescriptorPool(R.device, descPool, nullptr);
        vkDestroyCommandPool(R.device, pool, nullptr);
        vkDestroyBuffer(R.device, buf, nullptr);
        vkFreeMemory(R.device, bufMem, nullptr);
        vkDestroyImageView(R.device, view, nullptr);
        vkDestroyImage(R.device, img, nullptr);
        vkFreeMemory(R.device, imgMem, nullptr);
    };

    if (!ok)
    {
        lucent::error("draw", "EDRAM reinterpretation self-test: could not"
            " create its image, buffer, command pool or descriptor set --"
            " NOTHING was tested");
        destroy();
        return false;
    }

    VkDescriptorImageInfo ii{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(R.device, 1, &w, 0, nullptr);

    uint32_t failures = 0;
    for (uint32_t c = 0; c < kSelfTestCases; ++c)
    {
        const SelfTestCase& tc = kCases[c];
        // The input, checked against what it is meant to MEAN before it is used
        // for anything.
        bool inputOk = true;
        // A chained case's input is already in the staging buffer: it is the
        // previous case's readback, which is the whole point.
        if (!tc.chain)
        {
            void* map = nullptr;
            vkMapMemory(R.device, bufMem, 0, 8, 0, &map);
            auto* dst = static_cast<uint16_t*>(map);
            for (uint32_t k = 0; k < 4; ++k)
            {
                dst[k] = tc.in[k];
                const float decoded = HalfToFloat(tc.in[k]);
                if (decoded != tc.inValue[k])
                {
                    lucent::error("draw", "EDRAM reinterpretation self-test:"
                        " case '{}' component {} names the float16 bits {:#06x},"
                        " which decode to {} and not the {} the case claims --"
                        " the TEST is wrong, not the shader", tc.name, k,
                        tc.in[k], decoded, tc.inValue[k]);
                    inputOk = false;
                }
            }
            vkUnmapMemory(R.device, bufMem);
        }
        if (!inputOk)
        {
            ++failures;
            continue;
        }

        vkResetCommandPool(R.device, pool, 0);
        VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &cbi);

        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = img;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &bar);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(cmd, buf, img,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &bar);

        ReinterpretPushConstants pc{};
        pc.extent[0] = 1;
        pc.extent[1] = 1;
        pc.srcFormat = tc.from;
        pc.dstFormat = tc.to;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P.reinterpretPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                P.reinterpretLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, P.reinterpretLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, 1, 1, 1);

        bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &bar);
        vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               buf, 1, &region);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(R.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS ||
            vkQueueWaitIdle(R.queue) != VK_SUCCESS)
        {
            lucent::error("draw", "EDRAM reinterpretation self-test: case '{}'"
                " could not be submitted -- it was NOT tested", tc.name);
            ++failures;
            continue;
        }

        float got[4]{};
        {
            void* map = nullptr;
            vkMapMemory(R.device, bufMem, 0, 8, 0, &map);
            const auto* src = static_cast<const uint16_t*>(map);
            for (uint32_t k = 0; k < 4; ++k)
                got[k] = HalfToFloat(src[k]);
            vkUnmapMemory(R.device, bufMem);
        }
        // The tolerance is the container's, not a fudge factor: a float16 holds
        // 11 significant bits, so 2/3 comes back as 0.66650390625 and 31.875
        // comes back exactly. A relative 2^-10 admits the first and would still
        // reject 1.0 standing in for 31.875 by a factor of 30.
        bool caseOk = true;
        for (uint32_t k = 0; k < 4; ++k)
        {
            const float tol = std::max(1.0f, std::fabs(tc.expect[k])) * (1.0f / 1024.0f);
            if (std::fabs(got[k] - tc.expect[k]) > tol)
                caseOk = false;
        }
        if (!caseOk)
        {
            lucent::error("draw", "EDRAM reinterpretation self-test FAILED:"
                " '{}' turned ({}, {}, {}, {}) into ({}, {}, {}, {}), expected"
                " ({}, {}, {}, {})", tc.name, tc.inValue[0], tc.inValue[1],
                tc.inValue[2], tc.inValue[3], got[0], got[1], got[2], got[3],
                tc.expect[0], tc.expect[1], tc.expect[2], tc.expect[3]);
            ++failures;
        }
        else
        {
            lucent::info("draw", "EDRAM reinterpretation self-test: '{}' ok --"
                " ({}, {}, {}, {}) -> ({}, {}, {}, {})", tc.name, tc.inValue[0],
                tc.inValue[1], tc.inValue[2], tc.inValue[3], got[0], got[1],
                got[2], got[3]);
        }
    }

    destroy();
    if (failures == 0)
        lucent::info("draw", "EDRAM reinterpretation self-test: {} of {} cases"
            " passed on this GPU, including the pair that must not change"
            " anything", kSelfTestCases, kSelfTestCases);
    else
        lucent::error("draw", "EDRAM reinterpretation self-test: {} of {} cases"
            " FAILED -- do not trust a frame rendered with GEARS_DRAW_REINTERP=1"
            " until this passes", failures, kSelfTestCases);
    return failures == 0;
}


void RenderTargetCache::ReportReinterpretation(bool enabled) const
{
    if (!enabled)
    {
        // Silence here would read as "no format change happened". The frame's
        // format changes are known before any of this runs, so say how many
        // went unconverted.
        uint32_t changes = 0;
        for (const auto& [base, fmts] : formatsPerBase)
        {
            std::set<uint32_t> storage;
            for (uint32_t f : fmts)
                storage.insert(StorageColorFormat(f));
            if (storage.size() > 1)
                ++changes;
        }
        if (changes != 0)
            lucent::info("draw", "frame EDRAM reinterpretation: OFF"
                " (GEARS_DRAW_REINTERP=1 enables it) -- {} surface(s) are"
                " declared under more than one storage format this frame and"
                " every such change was left unconverted", changes);
        return;
    }
    lucent::Line line;
    line.add("frame EDRAM reinterpretation: {} converted", reinterpretsDone);
    for (uint64_t p : reinterpretPairs)
        line.add(" {}->{}", ColorFormatName(uint32_t(p >> 32)),
                 ColorFormatName(uint32_t(p)));
    if (reinterpretsRefused != 0)
    {
        line.add(", {} REFUSED (a format pair this pass does not convert;"
                 " those surfaces keep the value they held)", reinterpretsRefused);
        for (uint64_t p : reinterpretRefusedPairs)
            line.add(" {}->{}", ColorFormatName(uint32_t(p >> 32)),
                     ColorFormatName(uint32_t(p)));
    }
    if (reinterpretsOutOfSets != 0)
        line.add(", {} skipped for want of a descriptor set or a pipeline",
                 reinterpretsOutOfSets);
    // The deliberate non-conversions. Reported with their pairs because this is
    // the pass's main behaviour now, not an exception: a reader who sees only
    // "3 converted" cannot tell a frame with three format changes from one with
    // eight where five were correctly declined.
    if (reinterpretsNotRead != 0)
    {
        line.add(", {} NOT converted because the draw meeting the format change"
                 " does not read the destination (no blending, so it never sees"
                 " the old bits -- converting would rewrite pixels it does not"
                 " cover)", reinterpretsNotRead);
        for (uint64_t p : reinterpretNotReadPairs)
            line.add(" {}->{}", ColorFormatName(uint32_t(p >> 32)),
                     ColorFormatName(uint32_t(p)));
    }
    // A format change met by a draw that writes no colour at all. It is neither
    // a conversion nor a declined one: the surface keeps its contents AND its
    // format, and the next draw that does read the destination converts then.
    if (reinterpretsNoWrite != 0)
        line.add(", {} left alone because the draw meeting the format change"
                 " writes NO COLOUR (mask 0 or no fragment stage), so it neither"
                 " reads the old bits nor replaces them -- the surface keeps its"
                 " previous format until a draw that does write arrives",
                 reinterpretsNoWrite);
    // GEARS_DRAW_NOCONVERT. ALWAYS reported when the knob is set, INCLUDING at
    // zero: a control arm that matched no pair and one that changed nothing are
    // the same silence otherwise, and this arm exists to be believed.
    if (!lucent::config::text("DRAW_NOCONVERT").empty())
    {
        line.add("; GEARS_DRAW_NOCONVERT={} suppressed {} conversion(s)",
                 lucent::config::text("DRAW_NOCONVERT"), reinterpretsSuppressed);
        for (uint64_t p : reinterpretSuppressedPairs)
            line.add(" {}->{}", ColorFormatName(uint32_t(p >> 32)),
                     ColorFormatName(uint32_t(p)));
        if (reinterpretsSuppressed == 0)
            line.add(" -- NONE, so this run is the same as the default and says"
                     " nothing about any conversion");
    }
    if (reinterpretsDone == 0 && reinterpretsRefused == 0 &&
        reinterpretsOutOfSets == 0)
        line.add(" -- no surface changed storage format this frame, so this"
                 " pass could not have altered the picture");
    line.flush(reinterpretsRefused != 0 || reinterpretsOutOfSets != 0
                   ? lucent::Level::Warn
                   : lucent::Level::Info,
               "draw");
}

} // namespace gears::draw
