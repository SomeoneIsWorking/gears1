#pragma once

// Descriptor set layouts, pipeline layouts, the rectangle-list geometry shaders
// and the graphics pipelines -- everything a draw needs built before it can be
// recorded, cached so it is built once per run rather than once per frame.
//
// Building these per frame is what made a frame cost ~300 ms while the GPU work
// inside it was 6 ms. The cache is keyed on exactly what the object bakes in and
// no more; where that key is subtle the reason is on the function.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_formats.h"
#include "gpu_draw_renderer.h"
#include "gpu_draw_xlate.h"

namespace gears::draw
{

struct PipelineCache
{
    PipelineCache(Renderer &r, RendererPersistent &p)
        : R(r), P(p), texLayouts(p.texLayouts), pipeLayouts(p.pipeLayouts),
          geomShaders(p.geomShaders), pipelines(p.pipelines)
    {
    }

    Renderer &R;
    RendererPersistent &P;
    std::map<std::string, VkDescriptorSetLayout> &texLayouts;
    std::map<std::pair<std::string, std::string>, VkPipelineLayout> &pipeLayouts;
    std::map<GeometryShaderKey, VkShaderModule> &geomShaders;
    decltype(RendererPersistent::pipelines) &pipelines;

    // Rectangle-list draws seen, and how many were expanded by a geometry
    // shader. Reported by the frame census: a rectangle drawn as a bare triangle
    // list is missing half of itself, so the two numbers have to be comparable.
    uint32_t rectDraws = 0, rectDrawsExpanded = 0;
    // How many pipelines this frame built, and how many of those the guest
    // asked to STENCIL TEST. Both, always: a bare "0 use stencil" is
    // indistinguishable from a counter nobody incremented.
    uint32_t pipelinesBuilt = 0, pipelinesWithStencil = 0;

    // What building pipelines cost this run, accumulated inside GetPipeline so
    // the number covers the misses and not the lookups. The frame timing line
    // subtracts it from the state phase, so it has to be measured where it is
    // actually spent.
    double msPipeline = 0;

    // Creates set 0 and set 1, which every pipeline layout shares. Returns false
    // if either could not be created -- there is no drawing without them.
    bool Build();

    bool MakeSetLayout(const std::vector<VkDescriptorSetLayoutBinding> &b,
                       VkDescriptorSetLayout &l);

    std::string TexSignature(const ShaderXlate &x, VkShaderStageFlags stage);

    bool GetTexLayout(const ShaderXlate &x, VkShaderStageFlags stage, VkDescriptorSetLayout &out);

    bool GetPipeLayout(const ShaderXlate &vsX, const ShaderXlate &psX,
                       VkDescriptorSetLayout &outVsTex, VkDescriptorSetLayout &outPsTex,
                       VkPipelineLayout &out);

    bool GetRectGeomShader(uint64_t vsModification, VkShaderModule &out);

    bool GetPointGeomShader(uint64_t vsModification, uint64_t psModification, VkShaderModule &out);

    bool GetPipeline(VkShaderModule vsMod, VkShaderModule psMod, VkShaderModule gsMod,
                     uint32_t primType, const OutputMergerState &om, VkRenderPass renderPass,
                     VkSampleCountFlagBits samples, VkPipelineLayout pipeLayout, VkPipeline &out);
};

} // namespace gears::draw
