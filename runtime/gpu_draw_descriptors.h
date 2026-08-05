#pragma once

// A draw's four descriptor sets, and the per-frame cache that stops most draws
// building theirs at all.
//
//   set 0  the guest-memory SSBO
//   set 1  the five uniform blocks
//   set 2  the vertex shader's textures and samplers
//   set 3  the pixel shader's
//
// ONLY SETS 2 AND 3 ARE CACHED. Sets 0 and 1 carry this draw's uniform blocks,
// which are genuinely per draw (its transform, its constants) -- keyed together
// with the texture sets they produced 722 distinct groups for 722 draws. The
// texture sets are the ones draws share, and they are also the expensive ones:
// building them is what resolved 5224 texture bindings a frame.
//
// The key is CONTENT, not identity: the shader pair, the six fetch-constant
// dwords behind every texture and sampler binding the two shaders declare, and
// the resolve generation. Two draws agreeing on all of that cannot want
// different descriptors. An earlier attempt keyed on the uniform cache's
// register-snapshot POINTER and hit 0 times in 722 draws -- the guest rewrites
// registers between draws, so identity never matches even when the bindings do.
//
// The pool is reset at the top of each frame, so the cache lives for one frame.
// That is where the win is anyway.

#include <array>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_renderer.h"
#include "gpu_draw_textures.h"
#include "gpu_draw_uniforms.h"
#include "gpu_draw_xlate.h"

namespace gears::draw
{

struct DescriptorBuilder
{
    DescriptorBuilder(Renderer& r, RendererPersistent& p, TextureBinder& tb,
                      TextureUploader& tx)
        : R(r), P(p), TB(tb), TX(tx) { writes.reserve(32); }

    Renderer& R;
    RendererPersistent& P;
    TextureBinder& TB;
    TextureUploader& TX;

    // Set once per frame, before the first Build.
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorBufferInfo ssbo{};
    VkSampler stubSampler = VK_NULL_HANDLE;

    uint64_t hits = 0, builds = 0;
    size_t CacheSize() const { return cache.size(); }

    // msAlloc is vkAllocateDescriptorSets. msWrite is ASSEMBLING the writes and
    // submitting them; msUpdate is the submit alone, and it measures at ~0 -- so
    // the two together say whether the cost is ours or the driver's. It is ours.
    double msAlloc = 0, msWrite = 0, msUpdate = 0;

    // Fills `sets` with the four sets this draw binds. False means a descriptor
    // set could not be allocated and the caller must skip the draw -- binding
    // whatever was in `sets` before would draw with another draw's textures.
    bool Build(const uint32_t* regs, const FrameDrawItem& d, uint64_t resolveGeneration,
               const ShaderXlate& vsX, const ShaderXlate& psX,
               VkDescriptorSetLayout vsTexLayout, VkDescriptorSetLayout psTexLayout,
               const UniformCache& uc, VkDescriptorSet (&sets)[4]);

private:
    uint64_t KeyFor(const uint32_t* regs, const FrameDrawItem& d,
                    uint64_t resolveGeneration,
                    const ShaderXlate& vsX, const ShaderXlate& psX) const;

    std::unordered_map<uint64_t, std::array<VkDescriptorSet, 2>> cache;

    // Hoisted out of the per-draw path deliberately. A fresh vector plus a fresh
    // deque per draw is ~700 rounds of heap churn a frame for containers rebuilt
    // from scratch anyway. Cleared per draw; the deque still gives the pointer
    // stability vkUpdateDescriptorSets needs.
    std::vector<VkWriteDescriptorSet> writes;
    std::deque<VkDescriptorImageInfo> imgInfos;
    // The uniform infos the writes point at have to outlive the write call, so
    // this draw's are copied here rather than pointed to in the caller's frame.
    VkDescriptorBufferInfo biSys{}, biFvs{}, biFps{}, biBl{}, biFetch{};
};

} // namespace gears::draw
