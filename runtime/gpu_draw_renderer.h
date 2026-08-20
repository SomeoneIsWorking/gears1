#pragma once

// The renderer object and everything it owns across frames.
//
// These used to live in an anonymous namespace inside gpu_draw.cpp, which is the
// reason that file grew to 6500 lines: no phase of a frame could be moved to a
// translation unit of its own while the types it works on were file-local. They
// are here so it can be taken apart.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "gpu_draw_formats.h"
#include "gpu_draw_pixels.h"
#include "gpu_draw_xlate.h"
#include "gpu_scanout.h"

#include <lucent/log.h>

// Every renderer translation unit builds Vulkan objects and every one of them
// has to fail loudly rather than carry on with a null handle. One definition,
// so a TU cannot quietly use a laxer one.
#define VK_CHECK(expr)                                                                             \
    do                                                                                             \
    {                                                                                              \
        VkResult _r = (expr);                                                                      \
        if (_r != VK_SUCCESS)                                                                      \
        {                                                                                          \
            lucent::warn("draw", "{} -> {}", #expr, ::gears::draw::VkStr(_r));                     \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

namespace gears::draw
{

// A host image owned for the whole run: the guest's decoded textures, and the
// 1x1 stand-ins a binding falls back to when its fetch constant cannot be
// decoded (which is always COUNTED, never quietly substituted).
struct GuestTex
{
    VkImage image = 0;
    VkDeviceMemory mem = 0;
    VkImageView view = 0;
};
struct StubTex
{
    VkImage image = 0;
    VkDeviceMemory mem = 0;
    VkImageView view = 0;
};

// --- the render-target cache -------------------------------------------
// One host colour target per EDRAM colour surface, keyed by the surface's
// RB_COLOR_INFO.color_base. A UE3 frame on this title uses four of them: the
// 7e3 HDR world, the 8888 tonemap/UI output that is presented, a 16-bit float
// light accumulation buffer and a small 8888 one. Rendering them all into a
// single 8888 target -- what this backend did before -- means the tonemap
// draws, which run last, paint over the world they were supposed to composite.
//
// THE KEY IS THE BASE ALONE, not (base, format). An EDRAM tile base is a
// location in EDRAM; the format is how the draw currently interprets the bytes
// there, and a frame changes it. Measured on an Act 1 frame, base 0x2d0 is
// rendered as k_8_8_8_8 (103 draws), k_2_10_10_10_FLOAT (26),
// k_2_10_10_10_FLOAT_AS_16_16_16_16 (17) and k_16_16 (2) -- all the same
// surface, reinterpreted. Keying on the pair would split it across four host
// images that cannot see each other's output, which is the same defect one
// level down from the one this cache exists to fix. Instead each base gets ONE
// host image in a format wide enough for every format the frame uses there
// (HostFormatFor below).
//
// That container holds VALUES, and EDRAM holds BITS -- which agree only while
// the format does not change. When it does, the stored values must be pushed
// through the console's packing and read back under the new format, or a draw
// blends against a number the hardware never held: measured on
// walk_gameplay.gfr, guest draw 649 stores 1.0 into base 0x2d0 as k_2_10_10_10
// where the console stores the bits 0x3FF, which guest draw 650 reads back as
// the 7e3 float 31.875 and blends with. gpu_draw_reinterpret.cpp is that
// conversion; `storageFormat` below is the format the image's contents are
// currently in.
//
// The console also renders each surface in PREDICATED TILES, re-binding the
// same base once per tile. Tiles therefore ACCUMULATE into that one target: a
// surface is CLEARED the first time a frame touches it and LOADED every time
// after, which is what `begunThisFrame` tracks.
struct SurfaceTarget
{
    VkFormat hostFormat = VK_FORMAT_UNDEFINED;
    VkImage color = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
    // A plain 2D view of the same image, for the resolve compute shader. The
    // attachment view cannot serve: a storage image binding must be 2D, and
    // colorView is what the framebuffer holds.
    VkImageView storageView = VK_NULL_HANDLE;
    // ONE FRAMEBUFFER PER DEPTH BASE this surface is drawn with. A framebuffer
    // names its attachments, so a surface rendered against two different depth
    // targets needs two -- and this title renders surface 0x2d0 against both
    // the scene depth and the shadow atlas's.
    std::map<uint32_t, VkFramebuffer> fbs;
    bool begunThisFrame = false;
    uint32_t drawsThisFrame = 0;
    // The guest colour format (in STORAGE form -- see StorageColorFormat) the
    // image's contents are currently written in, or UINT32_MAX before the
    // frame's first draw into it. A draw declaring a different one needs the
    // contents converted first.
    uint32_t storageFormat = UINT32_MAX;
};

// A depth+stencil buffer, one per RB_DEPTH_INFO.depth_base. Same size, format
// and usage for all of them -- what differs is only that they are SEPARATE
// memories, which is what the console's EDRAM bases are.
struct DepthTarget
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    // The attachment view (both aspects), and the two single-aspect views the
    // depth resolve and the EDRAM aliasing pass sample through.
    VkImageView attachView = VK_NULL_HANDLE;
    VkImageView depthSampledView = VK_NULL_HANDLE;
    VkImageView stencilSampledView = VK_NULL_HANDLE;
    // Whether a pass has rendered into it THIS FRAME. The render pass that
    // clears is chosen by the COLOUR surface's first use, so a depth target
    // whose first use lands on a load pass is cleared explicitly instead.
    bool usedThisFrame = false;
};

// A resolve destination: the main-memory address the guest copies an EDRAM
// surface out to (RB_COPY_DEST_BASE), given a host image of its own. A later
// draw whose texture fetch constant names that address is sampling this frame's
// render target, and this is what it reads -- the real chain the tonemap pass
// needs, replacing the single global snapshot the RT link used to bind for
// every resolve destination at once.
struct ResolveTarget
{
    VkFormat hostFormat = VK_FORMAT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView storageView = VK_NULL_HANDLE; // 2D, for the resolve dispatch
    // SAMPLED VIEWS BY THE GUEST'S FETCH SWIZZLE, keyed on the raw 12-bit
    // field. A resolve destination is written with RB_COPY_DEST_INFO's
    // copy_dest_swap applied, and the consumer's fetch constant carries the
    // swizzle that reads it back -- on the hardware the two cancel. Serving
    // every binding the same unmapped `view` skipped that second half, so a
    // pass sampling a swapped target saw red and blue exchanged and wrote them
    // out that way (catalog #62: the whole frame from the motion-blur pass
    // onward). One view per distinct swizzle, because the swizzle is per
    // BINDING, not per target: the motion-blur pass asks for ZYXW on the scene
    // colour and XYZW on depth in the same draw.
    std::map<uint32_t, VkImageView> swizzleViews;
    uint32_t sourceBase = 0; // the EDRAM base it is a copy of
    uint32_t copies = 0;     // resolves into it this frame
    // A resolve destination is a REGION OF A TEXTURE, not a texture. This one
    // is the whole texture: the guest's RB_COPY_DEST_BASE/_PITCH describe it,
    // and a resolve whose base lands inside it writes at an offset rather than
    // minting an image of its own. That distinction is what assembles a frame
    // rendered in predicated tiles -- see catalog #32, where one 1280x720
    // half-float target was being split into two unrelated images because the
    // guest folds the second tile's row offset into RB_COPY_DEST_BASE.
    uint32_t base = 0;              // RB_COPY_DEST_BASE of the texture's first row
    uint32_t pitch = 0, height = 0; // in pixels, from RB_COPY_DEST_PITCH
    uint32_t bpp = 0;               // bytes per pixel of the guest destination format
    // The raw RB_COPY_DEST_INFO.copy_dest_format. Kept beside the derived bpp
    // because the cross-emulator pass comparison has to DECODE these bytes, and
    // one frame carries four-byte and eight-byte destinations under otherwise
    // identical keys -- reading a 16-bit-float buffer as k_8_8_8_8 makes a
    // plausible picture of the wrong pass.
    uint32_t guestFormat = 0;
    uint32_t width = 0, imageHeight = 0; // what the host image was created with
    bool everWritten = false;            // whether a resolve has landed in it yet
    // A DEPTH destination. It is not a copy of a colour surface: the guest reads
    // it back as k_24_8_FLOAT and its shaders take .x, so the host image holds
    // the depth as a float. R16G16B16A16_SFLOAT would be wrong here -- half
    // float carries about 11 mantissa bits near 1.0 against the guest's 20, so
    // depth would band where it matters most.
    bool isDepth = false;
};

// Everything a frame render needs that does not change between frames.
//
// RenderFrameImpl used to build all of this per call and tear it down again,
// which is why one frame cost ~300 ms while the GPU work inside it was 6 ms:
// 116 ms translating shaders it had already translated, 21 ms creating
// pipelines it had already created, plus the render target, render passes and
// descriptor set layouts. Held here, it is paid once.
//
// Note the coupling: a VkPipeline is only valid against a compatible
// VkRenderPass, and the cached texture views only outlive the frame because the
// images they view are owned here too -- so these cannot be made persistent one
// at a time.
struct RendererPersistent
{
    bool built = false;
    uint32_t width = 0, height = 0;

    // (microcode hash, modification, output clamped) -> translation and module.
    // The clamp belongs in the key because a widened host surface makes it a
    // property of the DRAW's guest colour format, not of the microcode.
    std::map<std::tuple<uint64_t, uint64_t, int>, draw::ShaderXlate> xlate;
    std::map<std::tuple<uint64_t, uint64_t, int>, VkShaderModule> modules;
    std::map<draw::RectangleGeometryShaderKey, VkShaderModule> geomShaders;

    std::map<std::string, VkDescriptorSetLayout> texLayouts;
    std::map<std::pair<std::string, std::string>, VkPipelineLayout> pipeLayouts;
    // The render pass is part of the key: a pipeline is only valid against a
    // render pass it is compatible with, and each colour format now has its own.
    std::map<std::tuple<VkShaderModule, VkShaderModule, VkShaderModule, uint32_t, OutputMergerState,
                        VkRenderPass>,
             VkPipeline>
        pipelines;
    VkDescriptorSetLayout set0 = VK_NULL_HANDLE, set1 = VK_NULL_HANDLE;

    // Guest textures, their views by fetch key, and their samplers. Keyed rather
    // than a flat list because an entry whose GUEST BYTES changed has to be found
    // and destroyed, not just replaced in the view map -- see the eviction in
    // uploadTexture.
    using GuestTextureKey = std::array<uint32_t, 6>;
    std::map<GuestTextureKey, GuestTex> guestTextures;
    std::map<GuestTextureKey, VkImageView> texCache;
    // The hash of the GUEST bytes each cached texture was built from. The cache key
    // is the fetch constant, which does not change when the guest overwrites the
    // pixels at the same address, so this is the only thing that can notice.
    std::map<GuestTextureKey, uint64_t> texContentHash;
    std::map<uint64_t, VkSampler> samplerCache;
    StubTex stub2D{}, stub3D{}, stubCube{};
    VkSampler stubSampler = VK_NULL_HANDLE;

    // The render-target cache: one host target per EDRAM colour surface, one
    // host image per resolve destination, and a pair of render passes (clear
    // and load) per host colour format.
    std::map<uint32_t, SurfaceTarget> surfaceTargets;                 // EDRAM color_base -> target
    std::map<uint32_t, ResolveTarget> resolveTargets;                 // RB_COPY_DEST_BASE -> image
    std::map<VkFormat, std::pair<VkRenderPass, VkRenderPass>> passes; // clear, load

    // The resolve compute pipeline. A resolve is not a blit: it applies the
    // guest's copy_dest_exp_bias and copy_dest_swap, which a blit cannot do.
    VkShaderModule resolveModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout resolveSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout resolveLayout = VK_NULL_HANDLE;
    VkPipeline resolvePipeline = VK_NULL_HANDLE;
    VkDescriptorPool resolveDescPool = VK_NULL_HANDLE;
    uint32_t resolveDescCapacity = 0;
    // EDRAM format reinterpretation: the pass run when a frame re-declares one
    // base under a different colour format, converting the stored values
    // through the bits the console would hold. See gpu_draw_reinterpret.cpp.
    VkShaderModule reinterpretModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout reinterpretSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout reinterpretLayout = VK_NULL_HANDLE;
    VkPipeline reinterpretPipeline = VK_NULL_HANDLE;
    // EDRAM COLOUR/DEPTH ALIASING (runtime/shaders/edram_depth_alias.comp):
    // the pass that writes the depth buffer's bits into a colour surface that
    // shares its EDRAM base. Separate from the reinterpretation pipeline
    // because it reads two images and writes a third.
    VkShaderModule depthAliasModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout depthAliasSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout depthAliasLayout = VK_NULL_HANDLE;
    VkPipeline depthAliasPipeline = VK_NULL_HANDLE;
    VkSampler depthAliasSampler = VK_NULL_HANDLE;
    VkDescriptorPool depthAliasDescPool = VK_NULL_HANDLE;
    VkDescriptorPool reinterpretDescPool = VK_NULL_HANDLE;
    uint32_t reinterpretDescCapacity = 0;
    bool reinterpretSelfTested = false;
    // The DEPTH resolve: its own pipeline, because its source is a sampled
    // image (a depth image cannot be a storage image) rather than a storage one.
    VkShaderModule resolveDepthModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout resolveDepthSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout resolveDepthLayout = VK_NULL_HANDLE;
    VkPipeline resolveDepthPipeline = VK_NULL_HANDLE;
    // The bound depth target's sampled views (see depthTargets below).
    VkImageView depthSampledView = VK_NULL_HANDLE;
    // The STENCIL aspect of the same depth image. A Vulkan image view carries
    // one aspect, so reading depth and stencil in one shader needs two views.
    VkImageView stencilSampledView = VK_NULL_HANDLE;

    // ONE DEPTH+STENCIL IMAGE PER RB_DEPTH_INFO.depth_base, not one for the
    // frame. The console addresses depth by an EDRAM base like it addresses
    // colour, and this title uses two: the scene renders against 0x0 and the
    // shadow atlas against 0x5a0. Sharing one image made the atlas passes
    // scribble over the scene's STENCIL, and the shadow-mask pass that tests it
    // was then rejected everywhere they had drawn -- three full-screen mask
    // draws rasterising and invoking no fragment shader at all, with the mask
    // surviving exactly where neither atlas tile had reached (catalog #91).
    //
    // The count of distinct bases was known and was read as harmless: about 1%
    // of draws use a base other than 0x0, which sized this as a refactor for a
    // rounding error in depth VALUES. What those 1% damage is not a value.
    //
    // `depth`, `depthView`, `depthSampledView` and `stencilSampledView` below
    // are the handles of the CURRENTLY BOUND target -- swapped by
    // RenderTargetCache::GetDepthTarget as the frame moves between bases -- so
    // every existing user (the mid-frame clear, the depth resolve, the aliasing
    // pass, the probes) keeps reading one place and gets the right image.
    std::map<uint32_t, DepthTarget> depthTargets; // depth_base -> target
    uint32_t boundDepthBase = UINT32_MAX;
    VkImage depth = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    GpuScanout scanout;

    // The guest-memory mirror the translated shaders fetch through. The buffer
    // is persistent; its CONTENTS are refreshed every frame, because guest
    // memory is exactly what changes between frames.
    VkBuffer ssbo = VK_NULL_HANDLE;
    VkDeviceMemory ssboMem = VK_NULL_HANDLE;
    VkDeviceSize ssboBytes = 0;

    // One persistently-mapped buffer the per-draw uniform blocks and expanded
    // index buffers are suballocated from, reset at the start of every frame.
    // They used to be a VkBuffer plus a VkDeviceMemory each -- five uniform
    // blocks per draw, created and destroyed every frame, which is 870
    // allocations on a 174-draw frame and where ~40 ms of a warm frame went.
    // It grows to the previous frame's high-water mark; a frame that outgrows
    // it mid-way falls back to standalone buffers for the remainder rather than
    // dropping draws, and the next frame is sized to fit.
    VkBuffer arena = VK_NULL_HANDLE;
    VkDeviceMemory arenaMem = VK_NULL_HANDLE;
    void *arenaMapped = nullptr;
    VkDeviceSize arenaBytes = 0;
    VkDeviceSize arenaHighWater = 0;

    // The frame's own command recording and pixel readback. These were created
    // and destroyed every frame too; the descriptor pool is RESET each frame
    // rather than rebuilt.
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    uint32_t descriptorPoolDraws = 0; // what it was sized for
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMem = VK_NULL_HANDLE;
    void *readbackMapped = nullptr;
    VkDeviceSize readbackBytes = 0;
    void *ssboMapped = nullptr;
};

// -------------------------------------------------------------------------
// Vulkan renderer, headless, one draw. Everything is torn down at the end; the
// hot draw fires once (the caller latches), so there is no pipeline caching to
// justify keeping the device alive.
struct Renderer
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    // Whether this renderer CREATED the device or adopted the presenter's. An
    // adopter must not destroy what it does not own, and destroying a device the
    // presenter is still drawing to would take the window down with it.
    bool ownsDevice = true;
    bool hasPipelineStats = false; // pipelineStatisticsQuery feature enabled
    bool hasGeometryShader = false;
    bool hasStorageImageWithoutFormat = false;
    // VkPhysicalDeviceFeatures.depthClamp -- see DeviceCapabilities::depthClamp
    // for what a device without it costs.
    bool hasDepthClamp = false;
    bool hasSamplerAnisotropy = false;
    float maxSamplerAnisotropy = 1.0f;
    VkDeviceSize uniformOffsetAlignment = 256;
    // THE HOST VIEWPORT IS NOT BOUNDED BY THE RENDER TARGET. With clipping
    // disabled (PA_CL_CLIP_CNTL.clip_disable) the guest expects no near/far or
    // side clipping at all, and the way that is emulated is a viewport LARGER
    // than the target -- up to 8192 -- with the vertex shader rescaling into it
    // through ndc_scale. Clamping it to the target's size while the shader
    // rescales for the large one shrinks every such draw by the ratio of the
    // two, which is how a full-quadrant depth+stencil write came out as a
    // 100x32 corner (catalog #91). Only the DEVICE's limit may clamp it, and
    // that clamp is reported rather than applied quietly.
    uint32_t maxViewportDim[2] = {16384, 16384};

    // Built on the first frame and retained as a capacity allocation. A raw
    // pointer rather than a unique_ptr so the type can stay
    // incomplete here: it names OutputMergerState and the texture structs,
    // which are declared further down.
    struct RendererPersistent *persistent = nullptr;

    bool Init();
    bool FindMemory(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t &out);
    bool MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buf, VkDeviceMemory &mem,
                    bool wantCached = false);
    bool RenderFrameImpl(const FrameDrawInputs &in);
    void EnsurePersistentCapacity(uint32_t requiredWidth, uint32_t requiredHeight);
    void ReleasePersistent();
};

} // namespace gears::draw
