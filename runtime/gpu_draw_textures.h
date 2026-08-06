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

    // A sampled view of a resolve destination carrying the guest's fetch
    // SWIZZLE. The destination was written with copy_dest_swap applied, and on
    // the hardware the consumer's swizzle reads it back -- the two cancel.
    // Binding one unmapped view for every consumer performed only the first
    // half. Views are cached per swizzle on the target and live as long as it.
    VkImageView ResolveTargetView(ResolveTarget& rt, uint32_t guestSwizzle);

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

// Which image a texture binding is served by -- the frame's own resolved
// surface, the guest's uploaded texture, or a stub -- and the census of what
// each frame's bindings named.
//
// The census is not decoration. "24 of 5278 bindings served by a resolve
// target" was once read as a defect and retracted, and the signed-fetch tally
// exists because this renderer leaves texture_swizzled_signs at zero: that is
// correct only while no fetch constant asks for a signed component, which is a
// claim about the title rather than about the renderer, so it is counted.
struct TextureBinder
{
    TextureBinder(RendererPersistent& p, TextureUploader& tx,
                  const std::set<uint32_t>& resolveDestinations,
                  const std::set<uint32_t>& depthResolveDestinations)
        : P(p), TX(tx), resolveDests(resolveDestinations),
          depthResolveDests(depthResolveDestinations) {}

    RendererPersistent& P;
    TextureUploader& TX;
    const std::set<uint32_t>& resolveDests;
    const std::set<uint32_t>& depthResolveDests;

    // GEARS_DRAW_NORT=1 restores the old behaviour of decoding a resolve
    // destination out of stale guest memory; GEARS_DRAW_NOTEX=1 restores the
    // stub-only frame. Both are A/B control arms, never fixes.
    bool rtLinkEnabled = true;
    bool texUploadEnabled = true;

    // Set before ANY binding of a draw is resolved, so a sampler of resolved
    // depth is attributed to the shader that reads it and not the previous one.
    uint64_t currentPsHash = 0;

    uint64_t bindsStub = 0;   // bindings served by a stub image
    uint64_t bindsRt = 0;     // bindings served by a resolve target
    uint64_t bindsGuest = 0;  // bindings served by real guest texture data
    uint64_t Binds() const { return bindsStub + bindsRt + bindsGuest; }

    // (depth resolve destination, pixel shader hash) -> bindings. Names the
    // shaders that decode resolved depth, so their microcode can be read.
    std::map<std::pair<uint32_t, uint64_t>, uint64_t> depthDestSamplers;
    std::map<uint32_t, uint64_t> fetchesWithSigns;  // sign bits -> bindings
    std::map<uint32_t, uint64_t> signedBases;       // base -> bindings wanting kSigned
    std::map<uint32_t, uint64_t> baseCount;         // fetch base address -> bindings
    std::map<uint32_t, uint64_t> baseRtCount;       // ... restricted to resolve destinations

    // What UPLOAD cost this frame. Accumulated here because this is the only
    // call site of TextureUploader::Upload -- reporting it under state+pipeline,
    // where its accumulator used to be declared, made state look twice its real
    // size and the descriptor writes a third of theirs.
    double msUpload = 0;

    // Picks the image view for one texture binding. The stub matching the
    // shader's declared image dimension is the floor.
    VkImageView SelectView(const uint32_t* regs, const ShaderTextureBinding& tb);
};

} // namespace gears::draw
