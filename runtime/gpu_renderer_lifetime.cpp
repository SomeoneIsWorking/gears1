#include "gpu_draw_renderer.h"

#include "gpu_draw_targets.h"

namespace gears::draw
{

Renderer::~Renderer()
{
    Shutdown();
}

void Renderer::Shutdown()
{
    frameSlots.Drain();
    ReleasePersistent();
    frameSlots.Release();
}

void Renderer::ReleasePersistent()
{
    if (!persistent)
        return;
    if (!frameSlots.Drain())
        return;
    RendererPersistent &P = *persistent;
    for (auto &[k, p] : P.pipelines)
        vkDestroyPipeline(device, p, nullptr);
    for (auto &[k, l] : P.pipeLayouts)
        vkDestroyPipelineLayout(device, l, nullptr);
    for (auto &[k, l] : P.texLayouts)
        vkDestroyDescriptorSetLayout(device, l, nullptr);
    for (auto &[k, m] : P.modules)
        vkDestroyShaderModule(device, m, nullptr);
    for (auto &[k, m] : P.geomShaders)
        if (m != VK_NULL_HANDLE)
            vkDestroyShaderModule(device, m, nullptr);
    for (auto &[k, sm] : P.samplerCache)
        vkDestroySampler(device, sm, nullptr);
    vkDestroyDescriptorSetLayout(device, P.set0, nullptr);
    vkDestroyDescriptorSetLayout(device, P.set1, nullptr);
    for (auto &[k, t] : P.guestTextures)
    {
        vkDestroyImageView(device, t.view, nullptr);
        vkDestroyImage(device, t.image, nullptr);
        vkFreeMemory(device, t.mem, nullptr);
    }
    for (StubTex *t : {&P.stub2D, &P.stub3D, &P.stubCube})
    {
        vkDestroyImageView(device, t->view, nullptr);
        vkDestroyImage(device, t->image, nullptr);
        vkFreeMemory(device, t->mem, nullptr);
    }
    vkDestroySampler(device, P.stubSampler, nullptr);
    auto releaseSurfaceTargets = [&](auto &targets)
    {
        for (auto &[k, s] : targets)
            ReleaseSurfaceTarget(device, s);
    };
    releaseSurfaceTargets(P.surfaceTargets);
    releaseSurfaceTargets(P.surfaceTargets2x);
    for (auto &[k, r] : P.resolveTargets)
    {
        vkDestroyImageView(device, r.view, nullptr);
        for (auto &[swz, v] : r.swizzleViews)
            vkDestroyImageView(device, v, nullptr);
        vkDestroyImageView(device, r.storageView, nullptr);
        vkDestroyImage(device, r.image, nullptr);
        vkFreeMemory(device, r.mem, nullptr);
    }
    for (auto &[k, rp] : P.passes)
    {
        vkDestroyRenderPass(device, rp.first, nullptr);
        vkDestroyRenderPass(device, rp.second, nullptr);
    }
    vkDestroyPipeline(device, P.resolveDepthPipeline, nullptr);
    vkDestroyPipeline(device, P.resolveDepth2xPipeline, nullptr);
    vkDestroyPipelineLayout(device, P.resolveDepthLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, P.resolveDepthSetLayout, nullptr);
    vkDestroyShaderModule(device, P.resolveDepthModule, nullptr);
    vkDestroyShaderModule(device, P.resolveDepth2xModule, nullptr);
    // The bound views are aliases of the targets below, so they are not
    // destroyed here; doing so would destroy each handle twice.

    vkDestroySampler(device, P.depthAliasSampler, nullptr);
    vkDestroyPipeline(device, P.depthAliasPipeline, nullptr);
    vkDestroyPipelineLayout(device, P.depthAliasLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, P.depthAliasSetLayout, nullptr);
    vkDestroyShaderModule(device, P.depthAliasModule, nullptr);
    vkDestroyPipeline(device, P.resolvePipeline, nullptr);
    vkDestroyPipelineLayout(device, P.resolveLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, P.resolveSetLayout, nullptr);
    vkDestroyShaderModule(device, P.resolveModule, nullptr);
    auto releaseDepthTargets = [&](auto &targets)
    {
        for (auto &[db, d] : targets)
        {
            vkDestroyImageView(device, d.stencilSampledView, nullptr);
            vkDestroyImageView(device, d.depthSampledView, nullptr);
            vkDestroyImageView(device, d.attachView, nullptr);
            vkDestroyImage(device, d.image, nullptr);
            vkFreeMemory(device, d.mem, nullptr);
        }
    };
    releaseDepthTargets(P.depthTargets);
    releaseDepthTargets(P.depthTargets2x);
    P.depthTargets.clear();
    P.depthTargets2x.clear();
    P.depth = VK_NULL_HANDLE;
    P.depthMem = VK_NULL_HANDLE;
    P.depthView = VK_NULL_HANDLE;
    P.depthSampledView = VK_NULL_HANDLE;
    P.stencilSampledView = VK_NULL_HANDLE;
    P.boundDepthBase = UINT32_MAX;
    P.boundDepthSamples = VK_SAMPLE_COUNT_1_BIT;
    P.scanout.Release(*this);
    delete persistent;
    persistent = nullptr;
}

} // namespace gears::draw
