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

#include <lucent/log.h>

// Every renderer translation unit builds Vulkan objects and every one of them
// has to fail loudly rather than carry on with a null handle. One definition,
// so a TU cannot quietly use a laxer one.
#define VK_CHECK(expr)                                                       \
    do {                                                                     \
        VkResult _r = (expr);                                                \
        if (_r != VK_SUCCESS) {                                              \
            lucent::warn("draw", "{} -> {}", #expr, ::gears::draw::VkStr(_r)); \
            return false;                                                    \
        }                                                                    \
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
// (HostFormatFor below), so a reinterpretation accumulates the way EDRAM does.
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
    VkFramebuffer fb = VK_NULL_HANDLE;
    bool begunThisFrame = false;
    uint32_t drawsThisFrame = 0;
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
    uint32_t sourceBase = 0; // the EDRAM base it is a copy of
    uint32_t copies = 0;     // resolves into it this frame
    // A resolve destination is a REGION OF A TEXTURE, not a texture. This one
    // is the whole texture: the guest's RB_COPY_DEST_BASE/_PITCH describe it,
    // and a resolve whose base lands inside it writes at an offset rather than
    // minting an image of its own. That distinction is what assembles a frame
    // rendered in predicated tiles -- see catalog #32, where one 1280x720
    // half-float target was being split into two unrelated images because the
    // guest folds the second tile's row offset into RB_COPY_DEST_BASE.
    uint32_t base = 0;       // RB_COPY_DEST_BASE of the texture's first row
    uint32_t pitch = 0, height = 0; // in pixels, from RB_COPY_DEST_PITCH
    uint32_t bpp = 0;        // bytes per pixel of the guest destination format
    uint32_t width = 0, imageHeight = 0; // what the host image was created with
    bool everWritten = false; // whether a resolve has landed in it yet
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
    std::map<std::tuple<VkShaderModule, VkShaderModule, VkShaderModule, uint32_t,
                        OutputMergerState, VkRenderPass>, VkPipeline> pipelines;
    VkDescriptorSetLayout set0 = VK_NULL_HANDLE, set1 = VK_NULL_HANDLE;

    // Guest textures, their views by fetch key, and their samplers. Keyed rather
    // than a flat list because an entry whose GUEST BYTES changed has to be found
    // and destroyed, not just replaced in the view map -- see the eviction in
    // uploadTexture.
    std::map<std::array<uint32_t, 4>, GuestTex> guestTextures;
    std::map<std::array<uint32_t, 4>, VkImageView> texCache;
    // The hash of the GUEST bytes each cached texture was built from. The cache key
    // is the fetch constant, which does not change when the guest overwrites the
    // pixels at the same address, so this is the only thing that can notice.
    std::map<std::array<uint32_t, 4>, uint64_t> texContentHash;
    std::map<uint64_t, VkSampler> samplerCache;
    StubTex stub2D{}, stub3D{}, stubCube{};
    VkSampler stubSampler = VK_NULL_HANDLE;

    // The render-target cache: one host target per EDRAM colour surface, one
    // host image per resolve destination, and a pair of render passes (clear
    // and load) per host colour format.
    std::map<uint32_t, SurfaceTarget> surfaceTargets; // EDRAM color_base -> target
    std::map<uint32_t, ResolveTarget> resolveTargets; // RB_COPY_DEST_BASE -> image
    std::map<VkFormat, std::pair<VkRenderPass, VkRenderPass>> passes; // clear, load

    // The resolve compute pipeline. A resolve is not a blit: it applies the
    // guest's copy_dest_exp_bias and copy_dest_swap, which a blit cannot do.
    VkShaderModule resolveModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout resolveSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout resolveLayout = VK_NULL_HANDLE;
    VkPipeline resolvePipeline = VK_NULL_HANDLE;
    VkDescriptorPool resolveDescPool = VK_NULL_HANDLE;
    uint32_t resolveDescCapacity = 0;
    // The DEPTH resolve: its own pipeline, because its source is a sampled
    // image (a depth image cannot be a storage image) rather than a storage one.
    VkShaderModule resolveDepthModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout resolveDepthSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout resolveDepthLayout = VK_NULL_HANDLE;
    VkPipeline resolveDepthPipeline = VK_NULL_HANDLE;
    VkImageView depthSampledView = VK_NULL_HANDLE;

    // Depth is shared by every surface for now; the frame's distinct
    // RB_DEPTH_INFO bases are counted per frame so the moment that stops being
    // faithful is visible rather than assumed.
    VkImage depth = VK_NULL_HANDLE; VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    // The image the finished frame is handed to the presenter in: 8888, one blit
    // from whatever surface the frame ended on.
    //
    // TWO OF THEM, ALTERNATING, and that is the whole point. The presenter runs on
    // its own thread and the renderer on another, so publishing the live render
    // target meant the presenter could blit a surface the renderer had already
    // started drawing the next frame into. Alternating means the image being shown
    // is never the image being written.
    VkImage presentStage[2]{};
    VkDeviceMemory presentStageMem[2]{};
    uint32_t presentStageIndex = 0;

    // The guest-memory mirror the translated shaders fetch through. The buffer
    // is persistent; its CONTENTS are refreshed every frame, because guest
    // memory is exactly what changes between frames.
    VkBuffer ssbo = VK_NULL_HANDLE; VkDeviceMemory ssboMem = VK_NULL_HANDLE;
    VkDeviceSize ssboBytes = 0;

    // One persistently-mapped buffer the per-draw uniform blocks and expanded
    // index buffers are suballocated from, reset at the start of every frame.
    // They used to be a VkBuffer plus a VkDeviceMemory each -- five uniform
    // blocks per draw, created and destroyed every frame, which is 870
    // allocations on a 174-draw frame and where ~40 ms of a warm frame went.
    // It grows to the previous frame's high-water mark; a frame that outgrows
    // it mid-way falls back to standalone buffers for the remainder rather than
    // dropping draws, and the next frame is sized to fit.
    VkBuffer arena = VK_NULL_HANDLE; VkDeviceMemory arenaMem = VK_NULL_HANDLE;
    void* arenaMapped = nullptr;
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
    VkBuffer readback = VK_NULL_HANDLE; VkDeviceMemory readbackMem = VK_NULL_HANDLE;
    void* readbackMapped = nullptr;
    VkDeviceSize readbackBytes = 0;
    void* ssboMapped = nullptr;
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
    VkDeviceSize uniformOffsetAlignment = 256;

    // Built on the first frame, reused by every frame after it; released only on
    // a target-size change. A raw pointer rather than a unique_ptr so the type can stay
    // incomplete here: it names OutputMergerState and the texture structs,
    // which are declared further down.
    struct RendererPersistent* persistent = nullptr;

    bool Init();
    bool FindMemory(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t& out);
    bool MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf,
                    VkDeviceMemory& mem, bool wantCached = false);
    bool RenderFrameImpl(const FrameDrawInputs& in);
    void ReleasePersistent();
};

} // namespace gears::draw
