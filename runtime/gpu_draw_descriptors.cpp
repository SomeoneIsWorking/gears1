// A draw's descriptor sets. gpu_draw_descriptors.h says what is cached and on
// what; this is the allocation and the writes.

#include "gpu_draw_descriptors.h"

#include <chrono>

namespace gears::draw
{

namespace
{
using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}
} // namespace

uint64_t DescriptorBuilder::KeyFor(const uint32_t* regs, const FrameDrawItem& d,
                                   uint64_t resolveGeneration,
                                   const ShaderXlate& vsX, const ShaderXlate& psX) const
{
    uint64_t key = 0xCBF29CE484222325ull;
    auto mix = [&key](uint32_t v) {
        key ^= v;
        key *= 0x100000001B3ull;
    };
    mix(uint32_t(d.vsHash)); mix(uint32_t(d.vsHash >> 32));
    mix(uint32_t(d.psHash)); mix(uint32_t(d.psHash >> 32));
    mix(uint32_t(resolveGeneration));
    auto mixBindings = [&](const ShaderXlate& x) {
        for (const auto& tb : x.textures)
            for (uint32_t k = 0; k < 6; ++k)
                mix(regs[0x4800 + (tb.fetchConstant & 31) * 6 + k]);
        for (const auto& sb : x.samplers)
            for (uint32_t k = 0; k < 6; ++k)
                mix(regs[0x4800 + (sb.fetchConstant & 31) * 6 + k]);
    };
    mixBindings(vsX);
    mixBindings(psX);
    return key;
}

bool DescriptorBuilder::Build(const uint32_t* regs, const FrameDrawItem& d,
                              uint64_t resolveGeneration,
                              const ShaderXlate& vsX, const ShaderXlate& psX,
                              VkDescriptorSetLayout vsTexLayout,
                              VkDescriptorSetLayout psTexLayout,
                              const UniformCache& uc, VkDescriptorSet (&sets)[4])
{
    biSys = uc.biSys; biFvs = uc.biFvs; biFps = uc.biFps;
    biBl = uc.biBl; biFetch = uc.biFetch;

    const uint64_t key = KeyFor(regs, d, resolveGeneration, vsX, psX);
    const auto cached = cache.find(key);
    const bool reuse = cached != cache.end();

    // Sets 0 and 1 are always this draw's own.
    {
        VkDescriptorSetLayout uboLayouts[2] = {P.set0, P.set1};
        VkDescriptorSetAllocateInfo uai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        uai.descriptorPool = pool;
        uai.descriptorSetCount = 2;
        uai.pSetLayouts = uboLayouts;
        const auto t0 = Clock::now();
        const VkResult r0 = vkAllocateDescriptorSets(R.device, &uai, sets);
        msAlloc += MsSince(t0);
        if (r0 != VK_SUCCESS)
            return false;
    }

    auto setBuf = [&](VkDescriptorSet s, uint32_t b, VkDescriptorType t,
                      VkDescriptorBufferInfo* bi) {
        VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws.dstSet = s; ws.dstBinding = b; ws.descriptorCount = 1;
        ws.descriptorType = t; ws.pBufferInfo = bi;
        writes.push_back(ws);
    };
    auto writeUniformSets = [&] {
        setBuf(sets[0], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &ssbo);
        setBuf(sets[1], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biSys);
        setBuf(sets[1], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biFvs);
        setBuf(sets[1], 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biFps);
        setBuf(sets[1], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biBl);
        setBuf(sets[1], 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biFetch);
    };

    if (reuse)
    {
        ++hits;
        sets[2] = cached->second[0]; sets[3] = cached->second[1];
        const auto t0 = Clock::now();
        writes.clear();
        writeUniformSets();
        vkUpdateDescriptorSets(R.device, uint32_t(writes.size()), writes.data(),
                               0, nullptr);
        msWrite += MsSince(t0);
        return true;
    }

    ++builds;
    {
        VkDescriptorSetLayout drawLayouts[2] = {vsTexLayout, psTexLayout};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = drawLayouts;
        const auto t0 = Clock::now();
        const VkResult allocResult = vkAllocateDescriptorSets(R.device, &ai, &sets[2]);
        msAlloc += MsSince(t0);
        if (allocResult != VK_SUCCESS)
            return false;
    }

    // Assembling the descriptor writes, as distinct from submitting them.
    // vkUpdateDescriptorSets itself measures at ~0 ms a frame, so if this region
    // is expensive the cost is in BUILDING the writes -- deriving a sampler per
    // binding, looking it up, pushing the structs -- and not in the driver.
    const auto writeBegin = Clock::now();
    writes.clear();
    imgInfos.clear();
    writeUniformSets();

    // One image per texture the shader declared, then one sampler each, exactly
    // in the translator's binding order.
    auto writeTextures = [&](const ShaderXlate& x, VkDescriptorSet set) {
        for (uint32_t i = 0; i < uint32_t(x.textures.size()); ++i)
        {
            VkDescriptorImageInfo ii{};
            ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            ii.imageView = TB.SelectView(regs, x.textures[i]);
            imgInfos.push_back(ii);
            VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            ws.dstSet = set; ws.dstBinding = i; ws.descriptorCount = 1;
            ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            ws.pImageInfo = &imgInfos.back();
            writes.push_back(ws);
        }
        for (uint32_t j = 0; j < x.samplerCount; ++j)
        {
            // Sampler state is the guest's: filters and clamp modes come from
            // the fetch constant this sampler binding names.
            VkDescriptorImageInfo si{};
            si.sampler = stubSampler;
            GuestSamplerState gs;
            if (j < x.samplers.size() &&
                DeriveSamplerState(&regs[0x4800 + (x.samplers[j].fetchConstant & 31) * 6],
                                   x.samplers[j], gs))
                si.sampler = TX.GetSampler(gs);
            imgInfos.push_back(si);
            VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            ws.dstSet = set; ws.dstBinding = uint32_t(x.textures.size()) + j;
            ws.descriptorCount = 1;
            ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            ws.pImageInfo = &imgInfos.back();
            writes.push_back(ws);
        }
    };
    writeTextures(vsX, sets[2]);
    writeTextures(psX, sets[3]);

    const auto updateBegin = Clock::now();
    if (!writes.empty())
        vkUpdateDescriptorSets(R.device, uint32_t(writes.size()), writes.data(),
                               0, nullptr);
    msUpdate += MsSince(updateBegin);
    msWrite += MsSince(writeBegin);
    cache.emplace(key, std::array<VkDescriptorSet, 2>{sets[2], sets[3]});
    return true;
}

} // namespace gears::draw
