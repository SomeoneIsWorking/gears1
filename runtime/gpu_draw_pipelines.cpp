// The pipeline and layout caches. gpu_draw_pipelines.h says what each one is
// keyed on; this is how each key is derived, which is the part that is subtle.

#include "gpu_draw_pipelines.h"

#include <chrono>
#include <cstring>
#include <tuple>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears::draw
{

using Clock = std::chrono::steady_clock;

bool PipelineCache::Build()
{
    const VkShaderStageFlags allStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayout &set0 = P.set0;
    VkDescriptorSetLayout &set1 = P.set1;
    if (!MakeSetLayout({{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages, nullptr}}, set0) ||
        !MakeSetLayout({{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
                        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
                        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
                        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
                        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr}},
                       set1))
        return false;

    return true;
}

bool PipelineCache::MakeSetLayout(const std::vector<VkDescriptorSetLayoutBinding> &b,
                                  VkDescriptorSetLayout &l)
{
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = uint32_t(b.size());
    ci.pBindings = b.empty() ? nullptr : b.data();
    VK_CHECK(vkCreateDescriptorSetLayout(R.device, &ci, nullptr, &l));
    return true;
}

std::string PipelineCache::TexSignature(const ShaderXlate &x, VkShaderStageFlags stage)
{
    std::string s;
    s.reserve(x.textures.size() * 2 + 8);
    for (const auto &t : x.textures)
        s.push_back(char('0' + (t.dimension & 3)));
    s.push_back('|');
    s += std::to_string(x.samplerCount);
    s.push_back('|');
    s += std::to_string(stage);
    return s;
}

bool PipelineCache::GetTexLayout(const ShaderXlate &x, VkShaderStageFlags stage,
                                 VkDescriptorSetLayout &out)
{
    const std::string key = TexSignature(x, stage);
    auto it = texLayouts.find(key);
    if (it != texLayouts.end())
    {
        out = it->second;
        return true;
    }
    std::vector<VkDescriptorSetLayoutBinding> b;
    b.reserve(x.textures.size() + x.samplerCount);
    for (uint32_t i = 0; i < uint32_t(x.textures.size()); ++i)
        b.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stage, nullptr});
    for (uint32_t j = 0; j < x.samplerCount; ++j)
        b.push_back(
            {uint32_t(x.textures.size()) + j, VK_DESCRIPTOR_TYPE_SAMPLER, 1, stage, nullptr});
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    if (!MakeSetLayout(b, l))
        return false;
    texLayouts[key] = l;
    out = l;
    return true;
}

bool PipelineCache::GetPipeLayout(const ShaderXlate &vsX, const ShaderXlate &psX,
                                  VkDescriptorSetLayout &outVsTex, VkDescriptorSetLayout &outPsTex,
                                  VkPipelineLayout &out)
{
    if (!GetTexLayout(vsX, VK_SHADER_STAGE_VERTEX_BIT, outVsTex) ||
        !GetTexLayout(psX, VK_SHADER_STAGE_FRAGMENT_BIT, outPsTex))
        return false;
    auto key = std::make_pair(TexSignature(vsX, VK_SHADER_STAGE_VERTEX_BIT),
                              TexSignature(psX, VK_SHADER_STAGE_FRAGMENT_BIT));
    auto it = pipeLayouts.find(key);
    if (it != pipeLayouts.end())
    {
        out = it->second;
        return true;
    }
    VkDescriptorSetLayout sets[4] = {P.set0, P.set1, outVsTex, outPsTex};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 4;
    pi.pSetLayouts = sets;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(R.device, &pi, nullptr, &pl) != VK_SUCCESS)
        return false;
    pipeLayouts[key] = pl;
    out = pl;
    return true;
}

bool PipelineCache::GetRectGeomShader(uint64_t vsModification, VkShaderModule &out)
{
    out = VK_NULL_HANDLE;
    if (!R.hasGeometryShader)
        return false;
    GeometryShaderKey key;
    if (!DeriveRectangleGeometryShaderKey(vsModification, key))
        return false;
    auto it = geomShaders.find(key);
    if (it != geomShaders.end())
    {
        out = it->second;
        return out != VK_NULL_HANDLE;
    }
    std::vector<uint32_t> spirv;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (BuildRectangleGeometryShader(key, spirv))
    {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spirv.size() * sizeof(uint32_t);
        ci.pCode = spirv.data();
        if (vkCreateShaderModule(R.device, &ci, nullptr, &mod) != VK_SUCCESS)
            mod = VK_NULL_HANDLE;
        else
            lucent::info("draw",
                         "rectangle geometry shader: {} interpolators,"
                         " {} clip, {} cull distances, {} SPIR-V words",
                         key.interpolatorCount, key.clipDistanceCount, key.cullDistanceCount,
                         spirv.size());
    }
    if (mod == VK_NULL_HANDLE)
        lucent::warn("draw", "rectangle geometry shader build failed");
    geomShaders[key] = mod;
    out = mod;
    return mod != VK_NULL_HANDLE;
}

bool PipelineCache::GetPointGeomShader(uint64_t vsModification, uint64_t psModification,
                                       VkShaderModule &out)
{
    out = VK_NULL_HANDLE;
    if (!R.hasGeometryShader)
        return false;
    GeometryShaderKey key;
    if (!DerivePointGeometryShaderKey(vsModification, psModification, key))
        return false;
    auto it = geomShaders.find(key);
    if (it != geomShaders.end())
    {
        out = it->second;
        return out != VK_NULL_HANDLE;
    }
    std::vector<uint32_t> spirv;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (BuildPointGeometryShader(key, spirv))
    {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spirv.size() * sizeof(uint32_t);
        ci.pCode = spirv.data();
        if (vkCreateShaderModule(R.device, &ci, nullptr, &mod) != VK_SUCCESS)
            mod = VK_NULL_HANDLE;
        else
            lucent::info("draw",
                         "point geometry shader: {} interpolators, "
                         "{} clip, {} cull distances, size {}, coordinates {}, {} SPIR-V words",
                         key.interpolatorCount, key.clipDistanceCount, key.cullDistanceCount,
                         key.hasPointSize, key.hasPointCoordinates, spirv.size());
    }
    if (mod == VK_NULL_HANDLE)
        lucent::warn("draw", "point geometry shader build failed");
    geomShaders[key] = mod;
    out = mod;
    return mod != VK_NULL_HANDLE;
}

bool PipelineCache::GetPipeline(VkShaderModule vsMod, VkShaderModule psMod, VkShaderModule gsMod,
                                uint32_t primType, const OutputMergerState &om,
                                VkRenderPass renderPass, VkPipelineLayout pipeLayout,
                                VkPipeline &out)
{
    auto key = std::make_tuple(vsMod, psMod, gsMod, primType, om, renderPass);
    auto it = pipelines.find(key);
    if (it != pipelines.end())
    {
        out = it->second;
        return true;
    }
    VkPipelineShaderStageCreateInfo stages[3]{};
    uint32_t stageCount = 0;
    stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[stageCount].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[stageCount].module = vsMod;
    stages[stageCount].pName = "main";
    ++stageCount;
    if (gsMod != VK_NULL_HANDLE)
    {
        stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[stageCount].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
        stages[stageCount].module = gsMod;
        stages[stageCount].pName = "main";
        ++stageCount;
    }
    // A null psMod means this draw has NO fragment stage. That is not an
    // optimisation: RB_MODECONTROL.edram_mode decides whether the pixel
    // shader runs at all, and a depth-only draw that runs one writes colour
    // the hardware would never have written. See the call site.
    if (psMod != VK_NULL_HANDLE)
    {
        stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[stageCount].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[stageCount].module = psMod;
        stages[stageCount].pName = "main";
        ++stageCount;
    }
    VkPipelineVertexInputStateCreateInfo vin{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = TopologyOf(primType);
    // Viewport and scissor are the GUEST's, per draw (PA_CL_VPORT_* /
    // PA_SC_*), so they are dynamic state rather than baked in -- a
    // host-fixed full-target viewport put this frame's geometry in the
    // top-left corner at the wrong scale.
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 3;
    dyn.pDynamicStates = dynStates;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    // PA_CL_CLIP_CNTL.clip_disable, as the guest asked for it. Clamping rather
    // than clipping is the only host way to say "do not clip against near and
    // far", and a full-screen fill written at z = -3.7e-09 is precisely the
    // primitive that needs it (catalog #91). GEARS_DRAW_NOCLAMP=1 is the control
    // arm: it renders as this did before, with those primitives clipped away.
    static const bool noClamp = lucent::config::flag("DRAW_NOCLAMP");
    rs.depthClampEnable = (om.depthClamp && !noClamp) ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // Xenia keeps this dynamic even when the current factors are zero: UE3
    // changes PA_SU_POLY_OFFSET_* between draws and Vulkan otherwise ignores
    // vkCmdSetDepthBias entirely. Every pass here has a depth attachment.
    rs.depthBiasEnable = VK_TRUE;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    static const bool noCull = lucent::config::flag("DRAW_NOCULL");
    static const bool invertFace = lucent::config::flag("DRAW_CULL_INVERT");
    if (om.polygonal && !noCull)
    {
        if (om.suScModeCntl & 1)
            rs.cullMode |= VK_CULL_MODE_FRONT_BIT;
        if (om.suScModeCntl & 2)
            rs.cullMode |= VK_CULL_MODE_BACK_BIT;
        // face: 0 = front is counter-clockwise. GEARS_DRAW_CULL_INVERT is a
        // control arm for the one thing not settled by the register: our
        // Y-flip lives in the shader's ndc_scale, and a Y flip reverses
        // screen-space winding.
        const bool cw = ((om.suScModeCntl >> 2) & 1) != 0;
        rs.frontFace =
            (cw != invertFace) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // Depth from RB_DEPTHCONTROL: z_enable +1, z_write_enable +2, zfunc +4.
    // GEARS_DRAW_NODEPTH=1 is a DIAGNOSTIC control arm only: it separates
    // "this draw is depth-rejected" from "this draw shades black". It is
    // never a fix -- the depth state below is the guest's own.
    static const bool noDepth = lucent::config::flag("DRAW_NODEPTH");
    ds.depthTestEnable = (!noDepth && ((om.depthControl >> 1) & 1)) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = ((om.depthControl >> 2) & 1) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = CompareOpOf(om.depthControl >> 4);
    // STENCIL, from RB_DEPTHCONTROL (enable +0, func +8, fail +11, zpass +14,
    // zfail +17, and the back-face set at +20..+31 when backface_enable is set)
    // and RB_STENCILREFMASK (ref, read mask, write mask, 8 bits each).
    //
    // Ignoring it was not neutral. This title's shadow passes MARK stencil and
    // the passes that follow are meant to be confined to the marked pixels;
    // with no stencil test every one of them ran over the whole screen
    // (catalog #91). GEARS_DRAW_NOSTENCIL=1 restores that as a control arm.
    static const bool noStencil = lucent::config::flag("DRAW_NOSTENCIL");
    ds.stencilTestEnable = (!noStencil && (om.depthControl & 1)) ? VK_TRUE : VK_FALSE;
    // Counted so "this title never uses stencil" and "nobody looked" cannot
    // read the same. Reported once per frame with its denominator.
    ++pipelinesBuilt;
    if (om.depthControl & 1)
        ++pipelinesWithStencil;
    ds.front.compareOp = CompareOpOf(om.depthControl >> 8);
    ds.front.failOp = StencilOpOf(om.depthControl >> 11);
    ds.front.passOp = StencilOpOf(om.depthControl >> 14);
    ds.front.depthFailOp = StencilOpOf(om.depthControl >> 17);
    ds.front.reference = om.stencilRefMask & 0xFF;
    ds.front.compareMask = (om.stencilRefMask >> 8) & 0xFF;
    ds.front.writeMask = (om.stencilRefMask >> 16) & 0xFF;
    // backface_enable (+7) selects whether the back-face set is its own state.
    // Without it the hardware uses the front-face ops for both, so mirroring is
    // the faithful default rather than a shortcut.
    if ((om.depthControl >> 7) & 1)
    {
        ds.back.compareOp = CompareOpOf(om.depthControl >> 20);
        ds.back.failOp = StencilOpOf(om.depthControl >> 23);
        ds.back.passOp = StencilOpOf(om.depthControl >> 26);
        ds.back.depthFailOp = StencilOpOf(om.depthControl >> 29);
        ds.back.reference = om.stencilRefMaskBf & 0xFF;
        ds.back.compareMask = (om.stencilRefMaskBf >> 8) & 0xFF;
        ds.back.writeMask = (om.stencilRefMaskBf >> 16) & 0xFF;
    }
    else
    {
        ds.back = ds.front;
    }
    // Colour write mask from RB_COLOR_MASK's RT0 nibble (r,g,b,a in bits
    // 0..3), and blending from RB_BLENDCONTROL0. A draw the guest masked off
    // entirely writes nothing, as on hardware.
    VkPipelineColorBlendAttachmentState cba{};
    if (om.colorMask & 1)
        cba.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if (om.colorMask & 2)
        cba.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if (om.colorMask & 4)
        cba.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (om.colorMask & 8)
        cba.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    const uint32_t cSrc = om.blend0 & 0x1F;
    const uint32_t cOp = (om.blend0 >> 5) & 0x7;
    const uint32_t cDst = (om.blend0 >> 8) & 0x1F;
    const uint32_t aSrc = (om.blend0 >> 16) & 0x1F;
    const uint32_t aOp = (om.blend0 >> 21) & 0x7;
    const uint32_t aDst = (om.blend0 >> 24) & 0x1F;
    const bool blendIsIdentity = BlendIsIdentity(om.blend0);
    // GEARS_DRAW_NOBLEND=1 is a DIAGNOSTIC control arm only, never a fix: it
    // disables blending so the pixel shader's own output lands in the target
    // unmodified. It separates "this draw shades black" from "this draw
    // shades something the blend equation multiplies away" -- every world
    // draw of this frame uses colour src factor kSrcAlpha, so an output
    // alpha of zero would erase it whatever its RGB is.
    static const bool noBlend = lucent::config::flag("DRAW_NOBLEND");
    cba.blendEnable = (noBlend || blendIsIdentity) ? VK_FALSE : VK_TRUE;
    cba.srcColorBlendFactor = BlendFactorOf(cSrc);
    cba.dstColorBlendFactor = BlendFactorOf(cDst);
    cba.colorBlendOp = BlendOpOf(cOp);
    cba.srcAlphaBlendFactor = BlendFactorOf(aSrc);
    cba.dstAlphaBlendFactor = BlendFactorOf(aDst);
    cba.alphaBlendOp = BlendOpOf(aOp);
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = stageCount;
    gp.pStages = stages;
    gp.pVertexInputState = &vin;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = pipeLayout;
    gp.renderPass = renderPass;
    gp.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    const auto tPipe = Clock::now();
    const VkResult pipeResult =
        vkCreateGraphicsPipelines(R.device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe);
    msPipeline += std::chrono::duration<double, std::milli>(Clock::now() - tPipe).count();
    if (pipeResult != VK_SUCCESS)
        return false;
    pipelines[key] = pipe;
    out = pipe;
    return true;
}

} // namespace gears::draw
