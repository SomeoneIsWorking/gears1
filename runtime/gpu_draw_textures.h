#pragma once

// Guest texture fetch constants -> host images, and guest sampler state -> host
// samplers.
//
// Every texture the frame samples is described by its own texture fetch
// constant. gpu_draw_xlate decodes one (Xenia's texture_util / texture_address /
// FormatInfo do the layout, detiling and format classification); this turns the
// result into a host image. A fetch whose format has no host mapping keeps the
// stub AND is counted with its reason -- nothing is ever substituted to make the
// frame look better, which is why the counters below are public: the frame
// report prints them, and a skip that is not reported is a texture silently
// replaced.
//
// This was 380 lines of locals and lambdas in the middle of RenderFrameImpl.
// Nothing about it is per-frame except the counters, so it is an object now.

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_renderer.h"
#include "gpu_draw_xlate.h"

namespace gears::draw
{

struct TextureUploader
{
    TextureUploader(Renderer& r, RendererPersistent& p, const FrameDrawInputs& inputs)
        : R(r), P(p), in(inputs), guestTextures(p.guestTextures),
          texCache(p.texCache), texContentHash(p.texContentHash),
          samplerCache(p.samplerCache) {}

    Renderer& R;
    RendererPersistent& P;
    const FrameDrawInputs& in;

    // Every texture the frame samples is described by its own texture fetch
    // constant. gpu_draw_xlate decodes one (Xenia's texture_util /
    // texture_address / FormatInfo do the layout, detiling and format
    // classification); here it becomes a host image. A fetch whose format has
    // no host mapping keeps the stub AND is counted with its reason -- nothing
    // is ever substituted to make the frame look better.
    std::map<std::array<uint32_t, 4>, GuestTex>& guestTextures;
    std::map<std::array<uint32_t, 4>, VkImageView>& texCache;
    std::map<std::array<uint32_t, 4>, uint64_t>& texContentHash;
    // MEASURING BEFORE FIXING. Whether the guest actually overwrites a texture
    // under a key we are caching has never been established -- the frontier entry
    // says "not yet observed", which meant "nobody looked". Eviction is only worth
    // building if this counts more than zero, and the denominator is what makes a
    // zero here mean anything.
    uint64_t texContentChecked = 0;
    uint64_t texContentChanged = 0;
    // What the staleness check itself costs, split from everything else that
    // happens per binding. "texture upload 28 ms with 0 textures uploaded" is not
    // an answer -- these two numbers say whether it is the hashing (bytes read
    // from guest memory) or the per-binding bookkeeping around it.
    uint64_t texHashBytes = 0;
    double msTexHash = 0;
    // The single slowest texture hash of the frame. A total says how much; this
    // says whether it is one enormous texture or all of them evenly, and those
    // want different fixes.
    double texHashWorstMs = 0;
    uint64_t texHashWorstBytes = 0;
    uint32_t texHashWorstBase = 0;
    uint64_t texBindingCalls = 0;
    // A texture is checked at most ONCE per frame. That is not just an
    // optimisation: this renderer reads guest memory at frame-render time, so
    // every binding in a frame sees the same bytes, and re-hashing per binding
    // answers the same question 5094 times instead of 26 (measured: 2.3 s of a
    // gameplay frame, which is why the check used to be off by default).
    std::set<std::array<uint32_t, 4>> texCheckedThisFrame;
    // Images an eviction replaced. They cannot be destroyed on the spot: draws
    // already recorded into THIS frame's command buffer may still reference them,
    // so they die after the frame's fence, with the other per-frame transients.
    std::vector<GuestTex> texRetired;
    std::vector<VkBuffer> stagingBufs;
    std::vector<VkDeviceMemory> stagingMems;
    struct PendingUpload
    {
        VkImage image; VkBuffer staging;
        uint32_t w, h, d, layers;
    };
    std::vector<PendingUpload> uploads;
    std::map<std::string, uint64_t> texSkips;  // reason -> bindings affected
    std::map<std::string, uint64_t> texFormatCensus;   // "fmt WxH dim tiled" summary
    std::map<std::string, uint64_t> texFormatBindings; // format name -> bindings
    std::set<std::array<uint32_t, 4>> texDistinct;
    uint64_t uploadedBytes = 0;
    std::map<uint64_t, VkSampler>& samplerCache;

    // Returns the host image view for one texture fetch constant, uploading it on
    // first sight, or VK_NULL_HANDLE with the reason counted.
    VkImageView Upload(const uint32_t* fetch6, uint32_t wantDim);

    // One sampler per distinct guest sampler state, built from the shader's
    // sampler binding resolved against its own fetch constant (clamp modes,
    // filters, anisotropy) -- not a fixed host sampler.
    VkSampler GetSampler(const GuestSamplerState& gs);
};

} // namespace gears::draw
