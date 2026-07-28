// The first real guest draw. See gpu_draw.h for scope.
//
// Nothing here is invented geometry or a stand-in shader: the two shader stages
// are Xenia's translation of the microcode the running title actually bound
// (the hot vertex/pixel pair), the vertices are read from the guest's own
// physical memory through the shared-memory SSBO exactly as the Xenos vfetch
// does, the constant UBOs are filled from the tracked register file with
// Xenia's own packing, and the draw parameters are the ones the DRAW_INDX
// packet carried. The only stub is texture0 (a 1x1 image), which this milestone
// explicitly allows -- the pixel shader samples a render target that does not
// exist yet.
#include "gpu_draw.h"

#include <lucent/config.h>
#include <lucent/log.h>

#ifdef GEARS_HAVE_GUEST_DRAW

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include <vulkan/vulkan.h>

#include "gpu_draw_xlate.h"

namespace gears
{
namespace
{

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

std::vector<uint8_t> g_frame; // last rendered R8G8B8A8 frame

const char* VkStr(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "INITIALIZATION_FAILED";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "FEATURE_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "EXTENSION_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "INCOMPATIBLE_DRIVER";
    default: return "VkResult";
    }
}

#define VK_CHECK(expr)                                                       \
    do {                                                                     \
        VkResult _r = (expr);                                                \
        if (_r != VK_SUCCESS) {                                              \
            lucent::warn("draw", "{} -> {}", #expr, VkStr(_r));              \
            return false;                                                    \
        }                                                                    \
    } while (0)

// Packs the float-constant UBO exactly as Xenia's UpdateBindings does: the used
// float constants, in ascending storage index, from the vertex half (0x4000) or
// pixel half (0x4400) of the ALU constant file.
std::vector<uint8_t> PackFloatConstants(const uint32_t* regDwords,
    const uint64_t bitmap[4], uint32_t floatCount, uint32_t regBase)
{
    std::vector<uint8_t> out(size_t(std::max(floatCount, 1u)) * 16, 0);
    uint8_t* w = out.data();
    for (uint32_t block = 0; block < 4; ++block) {
        uint64_t entry = bitmap[block];
        while (entry) {
            uint32_t idx = uint32_t(std::countr_zero(entry));
            entry &= ~(uint64_t(1) << idx);
            uint32_t constant = block * 64 + idx;
            std::memcpy(w, &regDwords[regBase + constant * 4], 16);
            w += 16;
        }
    }
    return out;
}

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
    bool hasPipelineStats = false; // pipelineStatisticsQuery feature enabled
    bool hasGeometryShader = false; // geometryShader feature enabled
    VkDeviceSize uniformOffsetAlignment = 256;

    // Built on the first frame, reused by every frame after it, released by
    // Shutdown. A raw pointer rather than a unique_ptr so the type can stay
    // incomplete here: it names OutputMergerState and the texture structs,
    // which are declared further down.
    struct RendererPersistent* persistent = nullptr;

    bool Init();
    void Shutdown();
    bool FindMemory(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t& out);
    bool MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf,
                    VkDeviceMemory& mem, bool wantCached = false);
    bool Render(const HotDrawInputs& in);
    bool RenderFrameImpl(const FrameDrawInputs& in);
    void ReleasePersistent();
};

bool Renderer::Init()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gears1-draw";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    const char* valLayer = "VK_LAYER_KHRONOS_validation";
    const char* dbgExt = "VK_EXT_debug_utils";
    if (std::getenv("GEARS_DRAW_VALIDATE"))
    {
        ii.enabledLayerCount = 1;
        ii.ppEnabledLayerNames = &valLayer;
        ii.enabledExtensionCount = 1;
        ii.ppEnabledExtensionNames = &dbgExt;
    }
    VkResult r = vkCreateInstance(&ii, nullptr, &instance);
    if (r != VK_SUCCESS)
    {
        lucent::warn("draw", "vkCreateInstance -> {}", VkStr(r));
        return false;
    }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (n == 0)
    {
        lucent::warn("draw", "no Vulkan physical device");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());
    int best = -1;
    for (VkPhysicalDevice cand : devs)
    {
        uint32_t fc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &fc, nullptr);
        std::vector<VkQueueFamilyProperties> fam(fc);
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &fc, fam.data());
        for (uint32_t i = 0; i < fc; ++i)
        {
            if (!(fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(cand, &p);
            int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                      : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > best)
            {
                best = score;
                physical = cand;
                queueFamily = i;
            }
            break;
        }
    }
    if (physical == VK_NULL_HANDLE)
    {
        lucent::warn("draw", "no graphics queue on any device");
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(physical, &p);
    // Suballocating uniform blocks out of one buffer means honouring the
    // device's uniform-buffer offset alignment; index offsets need 4.
    uniformOffsetAlignment = std::max<VkDeviceSize>(p.limits.minUniformBufferOffsetAlignment, 4);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    // The translated shaders read a storage buffer from the vertex stage; the
    // pixel stage samples. Reads never need the stores/atomics features, but
    // enable them if present so a driver that treats the SSBO as writable does
    // not reject the pipeline.
    VkPhysicalDeviceFeatures avail{};
    vkGetPhysicalDeviceFeatures(physical, &avail);
    VkPhysicalDeviceFeatures feats{};
    feats.vertexPipelineStoresAndAtomics = avail.vertexPipelineStoresAndAtomics;
    feats.fragmentStoresAndAtomics = avail.fragmentStoresAndAtomics;
    // Pipeline statistics let a draw report how far it actually got -- vertices
    // in, primitives after clipping, fragment shader invocations. Without it,
    // "this draw added no pixels" cannot be told apart from "this draw was
    // clipped away" or "this draw shaded black".
    feats.pipelineStatisticsQuery = avail.pipelineStatisticsQuery;
    hasPipelineStats = avail.pipelineStatisticsQuery != VK_FALSE;
    // A rectangle list gives three vertices and the hardware infers the fourth;
    // deriving it needs the shaded vertices, so it happens in a geometry shader.
    feats.geometryShader = avail.geometryShader;
    hasGeometryShader = avail.geometryShader != VK_FALSE;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.pEnabledFeatures = &feats;
    VkResult dr = vkCreateDevice(physical, &di, nullptr, &device);
    if (dr != VK_SUCCESS)
    {
        lucent::warn("draw", "vkCreateDevice -> {}", VkStr(dr));
        return false;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    if (std::getenv("GEARS_DRAW_VALIDATE"))
    {
        auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkCreateDebugUtilsMessengerEXT");
        if (create)
        {
            VkDebugUtilsMessengerCreateInfoEXT mi{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            mi.pfnUserCallback = +[](VkDebugUtilsMessageSeverityFlagBitsEXT,
                VkDebugUtilsMessageTypeFlagsEXT,
                const VkDebugUtilsMessengerCallbackDataEXT* d, void*) -> VkBool32 {
                lucent::warn("draw", "VK: {}", d->pMessage);
                return VK_FALSE;
            };
            create(instance, &mi, nullptr, &messenger);
        }
    }
    lucent::info("draw", "headless Vulkan device \"{}\" (queue family {})",
        p.deviceName, queueFamily);
    return true;
}

void Renderer::Shutdown()
{
    if (device)
    {
        vkDeviceWaitIdle(device);
        ReleasePersistent();
        vkDestroyDevice(device, nullptr);
    }
    if (messenger)
    {
        auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(instance, messenger, nullptr);
    }
    if (instance)
        vkDestroyInstance(instance, nullptr);
}

bool Renderer::FindMemory(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t& out)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & want) == want)
        {
            out = i;
            return true;
        }
    return false;
}

bool Renderer::MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                          VkBuffer& buf, VkDeviceMemory& mem, bool wantCached)
{
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &bi, nullptr, &buf));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buf, &req);
    uint32_t type = 0;
    // A buffer the CPU READS -- the pixel readback -- wants HOST_CACHED, or the
    // memcpy out of it runs at uncached-write-combined speed: 3.7 MiB took
    // ~15 ms that way. Everything else the CPU only writes, where coherent
    // uncached is the right choice.
    const VkMemoryPropertyFlags base =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!(wantCached &&
          FindMemory(req.memoryTypeBits, base | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, type)) &&
        !FindMemory(req.memoryTypeBits, base, type))
    {
        lucent::warn("draw", "no host-visible memory type for buffer");
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &mem));
    VK_CHECK(vkBindBufferMemory(device, buf, mem, 0));
    return true;
}

VkPrimitiveTopology TopologyOf(uint32_t primType)
{
    switch (primType)
    {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    // A rectangle list has no Vulkan topology of its own: its three vertices go
    // in as a triangle list and the geometry shader emits the two-triangle strip
    // (see getRectGeomShader). Anything else unhandled also falls here.
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// --- Xenos output-merger state -> Vulkan --------------------------------
// The output-merger state is per draw and lives in the register file the draw
// carries: RB_COLOR_MASK (0x2104), RB_BLENDCONTROL0 (0x2201) and RB_DEPTHCONTROL
// (0x2200). Ignoring it is not a cosmetic simplification: a scene frame issues
// depth-only passes with colour writes fully masked off, and rendering those
// with an unconditional RGBA write paints the frame black.
VkBlendFactor BlendFactorOf(uint32_t f)
{
    switch (f)
    {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOpOf(uint32_t op)
{
    switch (op)
    {
    case 0: return VK_BLEND_OP_ADD;
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp CompareOpOf(uint32_t f)
{
    switch (f & 7)
    {
    case 0: return VK_COMPARE_OP_NEVER;
    case 1: return VK_COMPARE_OP_LESS;
    case 2: return VK_COMPARE_OP_EQUAL;
    case 3: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 4: return VK_COMPARE_OP_GREATER;
    case 5: return VK_COMPARE_OP_NOT_EQUAL;
    case 6: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

// RB_COLOR_INFO.color_format (a xenos::ColorRenderTargetFormat) -> the host
// format the surface is rendered in. Ported from Xenia's
// VulkanRenderTargetCache::GetColorOwnDrawVulkanFormat.
//
// This is not a cosmetic choice. UE3 on the 360 renders its scene into a
// k_2_10_10_10_FLOAT (7e3) HDR surface whose values run to 32.0; rendering that
// into an 8888 UNORM host target clamps every highlight to 1.0 before the
// tonemap pass ever sees it, so the tonemap reads a flat white/black image.
VkFormat HostColorFormat(uint32_t colorFormat)
{
    switch (colorFormat)
    {
    case 0:  // k_8_8_8_8
    case 1:  // k_8_8_8_8_GAMMA -- gamma is applied on the way out, not stored
        return VK_FORMAT_R8G8B8A8_UNORM;
    case 2:  // k_2_10_10_10
    case 10: // k_2_10_10_10_AS_10_10_10_10
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 3:  // k_2_10_10_10_FLOAT (7e3, [0,32) RGB + unorm alpha)
    case 12: // k_2_10_10_10_FLOAT_AS_16_16_16_16
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 4:  // k_16_16 (fixed point -32..32)
        return VK_FORMAT_R16G16_SNORM;
    case 5:  // k_16_16_16_16 (fixed point -32..32)
        return VK_FORMAT_R16G16B16A16_SNORM;
    case 6:  // k_16_16_FLOAT
        return VK_FORMAT_R16G16_SFLOAT;
    case 7:  // k_16_16_16_16_FLOAT
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 14: // k_32_FLOAT
        return VK_FORMAT_R32_SFLOAT;
    case 15: // k_32_32_FLOAT
        return VK_FORMAT_R32G32_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

const char* ColorFormatName(uint32_t f)
{
    switch (f)
    {
    case 0: return "k_8_8_8_8";
    case 1: return "k_8_8_8_8_GAMMA";
    case 2: return "k_2_10_10_10";
    case 3: return "k_2_10_10_10_FLOAT";
    case 4: return "k_16_16";
    case 5: return "k_16_16_16_16";
    case 6: return "k_16_16_FLOAT";
    case 7: return "k_16_16_16_16_FLOAT";
    case 10: return "k_2_10_10_10_AS_10_10_10_10";
    case 12: return "k_2_10_10_10_FLOAT_AS_16_16_16_16";
    case 14: return "k_32_FLOAT";
    case 15: return "k_32_32_FLOAT";
    default: return "?";
    }
}

// The output-merger registers that select a pipeline, kept together so the
// pipeline cache is keyed on exactly the state the pipeline bakes in.
struct OutputMergerState
{
    uint32_t colorMask = 0;    // RB_COLOR_MASK
    uint32_t blend0 = 0;       // RB_BLENDCONTROL0
    uint32_t depthControl = 0; // RB_DEPTHCONTROL

    bool operator<(const OutputMergerState& o) const
    {
        return std::tie(colorMask, blend0, depthControl) <
               std::tie(o.colorMask, o.blend0, o.depthControl);
    }
};

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
    uint32_t sourceBase = 0; // the EDRAM base it is a copy of
    uint32_t copies = 0;     // resolves into it this frame
};

// The host format one EDRAM base is given, from every RB_COLOR_INFO.color_format
// the frame renders it with. A base used with a single format keeps that
// format's own host equivalent; a base the frame reinterprets needs a container
// that holds all of them, and R16G16B16A16_SFLOAT holds every 8/10/16-bit
// colour format the Xenos can render (a k_8_8_8_8 value lands in [0,1] exactly,
// a 7e3 HDR value up to 32 and a k_16_16 fixed-point value up to 32 are all far
// inside float16's range).
VkFormat HostFormatFor(const std::set<uint32_t>& formats, bool& mixedOut)
{
    mixedOut = false;
    if (formats.empty())
        return VK_FORMAT_UNDEFINED;
    if (formats.size() == 1)
        return HostColorFormat(*formats.begin());
    mixedOut = true;
    // A 32-bit float surface has no common container with a 4-channel one; the
    // caller reports that rather than this pretending otherwise.
    for (uint32_t f : formats)
        if (f == 14 || f == 15)
            return VK_FORMAT_R32G32_SFLOAT;
    return VK_FORMAT_R16G16B16A16_SFLOAT;
}

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

    // (microcode hash, modification) -> translation and module.
    std::map<std::pair<uint64_t, uint64_t>, draw::ShaderXlate> xlate;
    std::map<std::pair<uint64_t, uint64_t>, VkShaderModule> modules;
    std::map<draw::RectangleGeometryShaderKey, VkShaderModule> geomShaders;

    std::map<std::string, VkDescriptorSetLayout> texLayouts;
    std::map<std::pair<std::string, std::string>, VkPipelineLayout> pipeLayouts;
    // The render pass is part of the key: a pipeline is only valid against a
    // render pass it is compatible with, and each colour format now has its own.
    std::map<std::tuple<VkShaderModule, VkShaderModule, VkShaderModule, uint32_t,
                        OutputMergerState, VkRenderPass>, VkPipeline> pipelines;
    VkDescriptorSetLayout set0 = VK_NULL_HANDLE, set1 = VK_NULL_HANDLE;

    // Guest textures, their views by fetch key, and their samplers.
    std::vector<GuestTex> guestTextures;
    std::map<std::array<uint32_t, 4>, VkImageView> texCache;
    std::map<uint64_t, VkSampler> samplerCache;
    StubTex stub2D{}, stub3D{}, stubCube{};
    VkSampler stubSampler = VK_NULL_HANDLE;

    // The render-target cache: one host target per EDRAM colour surface, one
    // host image per resolve destination, and a pair of render passes (clear
    // and load) per host colour format.
    std::map<uint32_t, SurfaceTarget> surfaceTargets; // EDRAM color_base -> target
    std::map<uint32_t, ResolveTarget> resolveTargets; // RB_COPY_DEST_BASE -> image
    std::map<VkFormat, std::pair<VkRenderPass, VkRenderPass>> passes; // clear, load

    // Depth is shared by every surface for now; the frame's distinct
    // RB_DEPTH_INFO bases are counted per frame so the moment that stops being
    // faithful is visible rather than assumed.
    VkImage depth = VK_NULL_HANDLE; VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;

    // An 8888 staging image for readback, used only when the presented surface
    // is in some other host format.
    VkImage presentStage = VK_NULL_HANDLE;
    VkDeviceMemory presentStageMem = VK_NULL_HANDLE;

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

void Renderer::ReleasePersistent()
{
    if (!persistent)
        return;
    RendererPersistent& P = *persistent;
    for (auto& [k, p] : P.pipelines) vkDestroyPipeline(device, p, nullptr);
    for (auto& [k, l] : P.pipeLayouts) vkDestroyPipelineLayout(device, l, nullptr);
    for (auto& [k, l] : P.texLayouts) vkDestroyDescriptorSetLayout(device, l, nullptr);
    for (auto& [k, m] : P.modules) vkDestroyShaderModule(device, m, nullptr);
    for (auto& [k, m] : P.geomShaders)
        if (m != VK_NULL_HANDLE) vkDestroyShaderModule(device, m, nullptr);
    for (auto& [k, sm] : P.samplerCache) vkDestroySampler(device, sm, nullptr);
    vkDestroyDescriptorSetLayout(device, P.set0, nullptr);
    vkDestroyDescriptorSetLayout(device, P.set1, nullptr);
    for (GuestTex& t : P.guestTextures)
    {
        vkDestroyImageView(device, t.view, nullptr);
        vkDestroyImage(device, t.image, nullptr);
        vkFreeMemory(device, t.mem, nullptr);
    }
    for (StubTex* t : {&P.stub2D, &P.stub3D, &P.stubCube})
    {
        vkDestroyImageView(device, t->view, nullptr);
        vkDestroyImage(device, t->image, nullptr);
        vkFreeMemory(device, t->mem, nullptr);
    }
    vkDestroySampler(device, P.stubSampler, nullptr);
    for (auto& [k, s] : P.surfaceTargets)
    {
        vkDestroyFramebuffer(device, s.fb, nullptr);
        vkDestroyImageView(device, s.colorView, nullptr);
        vkDestroyImage(device, s.color, nullptr);
        vkFreeMemory(device, s.colorMem, nullptr);
    }
    for (auto& [k, r] : P.resolveTargets)
    {
        vkDestroyImageView(device, r.view, nullptr);
        vkDestroyImage(device, r.image, nullptr);
        vkFreeMemory(device, r.mem, nullptr);
    }
    for (auto& [k, rp] : P.passes)
    {
        vkDestroyRenderPass(device, rp.first, nullptr);
        vkDestroyRenderPass(device, rp.second, nullptr);
    }
    vkDestroyImageView(device, P.depthView, nullptr);
    vkDestroyImage(device, P.depth, nullptr); vkFreeMemory(device, P.depthMem, nullptr);
    vkDestroyImage(device, P.presentStage, nullptr);
    vkFreeMemory(device, P.presentStageMem, nullptr);
    if (P.ssboMapped)
        vkUnmapMemory(device, P.ssboMem);
    vkDestroyBuffer(device, P.ssbo, nullptr); vkFreeMemory(device, P.ssboMem, nullptr);
    if (P.arenaMapped)
        vkUnmapMemory(device, P.arenaMem);
    vkDestroyBuffer(device, P.arena, nullptr); vkFreeMemory(device, P.arenaMem, nullptr);
    if (P.readbackMapped)
        vkUnmapMemory(device, P.readbackMem);
    vkDestroyBuffer(device, P.readback, nullptr);
    vkFreeMemory(device, P.readbackMem, nullptr);
    vkDestroyDescriptorPool(device, P.descriptorPool, nullptr);
    vkDestroyFence(device, P.fence, nullptr);
    vkDestroyCommandPool(device, P.cmdPool, nullptr);
    delete persistent;
    persistent = nullptr;
}

// Xenos VGT_DRAW_INITIATOR prim_type names, for the frame census.
// "src ONE, dst ZERO, add" on both colour and alpha is the blend identity, so
// blending is only switched on when the guest actually asked for it. Shared
// between the pipeline builder and the per-draw diagnostic table, so the table
// reports the state the pipeline was actually built with.
bool BlendIsIdentity(uint32_t blend0)
{
    const uint32_t cSrc = blend0 & 0x1F, cOp = (blend0 >> 5) & 0x7;
    const uint32_t cDst = (blend0 >> 8) & 0x1F;
    const uint32_t aSrc = (blend0 >> 16) & 0x1F, aOp = (blend0 >> 21) & 0x7;
    const uint32_t aDst = (blend0 >> 24) & 0x1F;
    return cSrc == 1 && cDst == 0 && cOp == 0 && aSrc == 1 && aDst == 0 && aOp == 0;
}

const char* PrimName(uint32_t primType)
{
    switch (primType)
    {
    case 1: return "point_list";
    case 2: return "line_list";
    case 3: return "line_strip";
    case 4: return "triangle_list";
    case 5: return "triangle_fan";
    case 6: return "triangle_strip";
    case 7: return "triangle_w_wflags";
    case 8: return "rectangle_list";
    case 12: return "line_loop";
    case 13: return "quad_list";
    case 14: return "quad_strip";
    case 15: return "polygon";
    default: return "other";
    }
}

bool WritePpm(const std::filesystem::path& path, const uint8_t* rgba,
              uint32_t w, uint32_t h)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << "P6\n" << w << ' ' << h << "\n255\n";
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* src = rgba + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x)
        {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        f.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
    }
    return true;
}

bool Renderer::Render(const HotDrawInputs& in)
{
    // --- translate the two shaders (SPIR-V + float-constant maps) -----------
    draw::ShaderXlate vs, ps;
    if (!draw::TranslateHotPair(in.registerFile, in.vsUcode, in.vsUcodeSize, in.vsHash,
            in.psUcode, in.psUcodeSize, in.psHash, vs, ps))
        return false;

    auto makeModule = [&](const std::vector<uint8_t>& spirv, VkShaderModule& m) -> bool {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = spirv.size();
        mi.pCode = reinterpret_cast<const uint32_t*>(spirv.data());
        VK_CHECK(vkCreateShaderModule(device, &mi, nullptr, &m));
        return true;
    };
    VkShaderModule vsMod = VK_NULL_HANDLE, psMod = VK_NULL_HANDLE;
    if (!makeModule(vs.spirv, vsMod) || !makeModule(ps.spirv, psMod))
        return false;

    // --- buffers ------------------------------------------------------------
    const uint32_t* R = in.registerFile;

    // Shared-memory SSBO: a verbatim mirror of low guest physical memory. The
    // Xenos vfetch reads vertices from here by physical byte address; the shader
    // applies the fetch constant's endian swap, so the bytes are copied raw.
    VkBuffer ssbo = VK_NULL_HANDLE; VkDeviceMemory ssboMem = VK_NULL_HANDLE;
    if (!MakeBuffer(in.guestPhysicalMirrorBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            ssbo, ssboMem))
        return false;
    {
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, ssboMem, 0, in.guestPhysicalMirrorBytes, 0, &p));
        std::memcpy(p, in.guestBase, in.guestPhysicalMirrorBytes);
        vkUnmapMemory(device, ssboMem);
    }
    // Prove the geometry is the guest's own: read the vertex buffer the hot VS
    // fetches (vertex fetch constant #95 -> physical base) straight from the
    // mirror and report the four positions. FMT_32_32_32_32_FLOAT, big-endian,
    // stride 12 dwords. Expected: a full-screen NDC quad (catalog #24).
    {
        const uint32_t vf0 = R[0x48BE];
        const uint32_t vbase = (vf0 >> 2) << 2;
        if ((vf0 & 3) == 3 && vbase + 4 * 12 * 4 <= in.guestPhysicalMirrorBytes)
        {
            for (uint32_t v = 0; v < 4; ++v)
            {
                float pos[4];
                for (int c = 0; c < 4; ++c)
                {
                    uint32_t w;
                    std::memcpy(&w, in.guestBase + vbase + (v * 12 + c) * 4, 4);
                    w = __builtin_bswap32(w);
                    std::memcpy(&pos[c], &w, 4);
                }
                lucent::info("draw", "  guest vertex {} @ {:#x}: ({}, {}, {}, {})",
                    v, vbase + v * 12 * 4, pos[0], pos[1], pos[2], pos[3]);
            }
            // Second attribute (r1, vfetch offset 6, dwords 6..9 of vertex 0):
            // the pixel shader multiplies the texture sample by this interpolant.
            float a[4];
            for (int c = 0; c < 4; ++c)
            {
                uint32_t w;
                std::memcpy(&w, in.guestBase + vbase + (6 + c) * 4, 4);
                w = __builtin_bswap32(w);
                std::memcpy(&a[c], &w, 4);
            }
            lucent::debug("draw", "  vertex 0 attribute r1 = ({}, {}, {}, {})",
                a[0], a[1], a[2], a[3]);
        }
    }

    // Constant UBOs.
    auto ubo = [&](const void* data, size_t size, VkBuffer& b, VkDeviceMemory& m) -> bool {
        if (!MakeBuffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, b, m))
            return false;
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, m, 0, size, 0, &p));
        std::memcpy(p, data, size);
        vkUnmapMemory(device, m);
        return true;
    };

    std::vector<uint8_t> sysc = draw::DeriveSystemConstants(R);
    std::vector<uint8_t> floatVs = PackFloatConstants(R, vs.floatBitmap, vs.floatCount, 0x4000);
    std::vector<uint8_t> floatPs = PackFloatConstants(R, ps.floatBitmap, ps.floatCount, 0x4400);
    // Bool (8 dwords at 0x4900) + loop (32 dwords at 0x4908), contiguous.
    std::vector<uint8_t> boolLoop(sizeof(uint32_t) * (8 + 32));
    std::memcpy(boolLoop.data(), &R[0x4900], boolLoop.size());
    // Fetch file: 32 slots * 6 dwords at 0x4800.
    std::vector<uint8_t> fetch(sizeof(uint32_t) * 6 * 32);
    std::memcpy(fetch.data(), &R[0x4800], fetch.size());

    VkBuffer uSys = 0, uFvs = 0, uFps = 0, uBl = 0, uFetch = 0;
    VkDeviceMemory mSys = 0, mFvs = 0, mFps = 0, mBl = 0, mFetch = 0;
    if (!ubo(sysc.data(), sysc.size(), uSys, mSys) ||
        !ubo(floatVs.data(), floatVs.size(), uFvs, mFvs) ||
        !ubo(floatPs.data(), floatPs.size(), uFps, mFps) ||
        !ubo(boolLoop.data(), boolLoop.size(), uBl, mBl) ||
        !ubo(fetch.data(), fetch.size(), uFetch, mFetch))
        return false;

    // Index buffer: read the guest indices (ReadGuest32 semantics: guest memory
    // is big-endian, so byte-swap each dword to host order) into a real Vulkan
    // index buffer. gl_VertexIndex then carries the guest index values into the
    // shader's vfetch, exactly as the hardware feeds them.
    const uint32_t indexBytes = in.indexCount * 4;
    VkBuffer ibuf = 0; VkDeviceMemory ibufMem = 0;
    if (!MakeBuffer(std::max(indexBytes, 4u), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, ibuf, ibufMem))
        return false;
    {
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, ibufMem, 0, std::max(indexBytes, 4u), 0, &p));
        uint32_t* dst = static_cast<uint32_t*>(p);
        const uint8_t* base = in.guestBase + in.indexGuestBase;
        for (uint32_t i = 0; i < in.indexCount; ++i)
        {
            uint32_t v;
            std::memcpy(&v, base + i * 4, 4);
            if (in.indexIs32)
                v = __builtin_bswap32(v); // 8in32 guest -> host
            dst[i] = v;
        }
        vkUnmapMemory(device, ibufMem);
    }

    // --- stub texture0 (1x1) + sampler --------------------------------------
    // The pixel shader samples a render target that does not exist yet. A 1x1
    // image is explicitly allowed for this milestone. Xenia always binds a 2D
    // ARRAY view, so the stub matches that.
    VkImage tex = 0; VkDeviceMemory texMem = 0; VkImageView texView = 0; VkSampler samp = 0;
    {
        VkImageCreateInfo ti{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ti.imageType = VK_IMAGE_TYPE_2D;
        ti.format = VK_FORMAT_R8G8B8A8_UNORM;
        ti.extent = {1, 1, 1};
        ti.mipLevels = 1;
        ti.arrayLayers = 1;
        ti.samples = VK_SAMPLE_COUNT_1_BIT;
        ti.tiling = VK_IMAGE_TILING_OPTIMAL;
        ti.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ti.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &ti, nullptr, &tex));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, tex, &req);
        uint32_t type = 0;
        if (!FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
            FindMemory(req.memoryTypeBits, 0, type);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &texMem));
        VK_CHECK(vkBindImageMemory(device, tex, texMem, 0));

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = tex;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &vi, nullptr, &texView));

        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(device, &si, nullptr, &samp));
    }

    // --- offscreen colour target -------------------------------------------
    VkImage color = 0; VkDeviceMemory colorMem = 0; VkImageView colorView = 0;
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {kWidth, kHeight, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(device, &ci, nullptr, &color));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, color, &req);
        uint32_t type = 0;
        if (!FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
            FindMemory(req.memoryTypeBits, 0, type);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &colorMem));
        VK_CHECK(vkBindImageMemory(device, color, colorMem, 0));

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = color;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &vi, nullptr, &colorView));
    }

    // --- render pass + framebuffer -----------------------------------------
    VkRenderPass renderPass = 0;
    {
        VkAttachmentDescription att{};
        att.format = VK_FORMAT_R8G8B8A8_UNORM;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        // Make the colour writes available to the subsequent image->buffer copy:
        // without this dependency the copy may read the attachment before the
        // fragment shader's writes land, yielding a zeroed readback.
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = 0;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 2;
        rp.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(device, &rp, nullptr, &renderPass));
    }
    VkFramebuffer fb = 0;
    {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = renderPass;
        fi.attachmentCount = 1;
        fi.pAttachments = &colorView;
        fi.width = kWidth;
        fi.height = kHeight;
        fi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device, &fi, nullptr, &fb));
    }

    // --- descriptor set layouts (sets 0..3, matching the SPIR-V) ------------
    auto makeSetLayout = [&](const std::vector<VkDescriptorSetLayoutBinding>& b,
                             VkDescriptorSetLayout& l) -> bool {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(b.size());
        ci.pBindings = b.empty() ? nullptr : b.data();
        VK_CHECK(vkCreateDescriptorSetLayout(device, &ci, nullptr, &l));
        return true;
    };
    const VkShaderStageFlags allStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayout set0 = 0, set1 = 0, set2 = 0, set3 = 0;
    if (!makeSetLayout({{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages, nullptr}}, set0))
        return false;
    if (!makeSetLayout({
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
            {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
            {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
            {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr}}, set1))
        return false;
    if (!makeSetLayout({}, set2))
        return false;
    if (!makeSetLayout({
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}, set3))
        return false;

    VkDescriptorSetLayout setLayouts[4] = {set0, set1, set2, set3};
    VkPipelineLayout pipeLayout = 0;
    {
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 4;
        pi.pSetLayouts = setLayouts;
        VK_CHECK(vkCreatePipelineLayout(device, &pi, nullptr, &pipeLayout));
    }

    // --- graphics pipeline --------------------------------------------------
    VkPipeline pipeline = 0;
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vsMod;
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = psMod;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vin{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = TopologyOf(in.primType);

        VkViewport vp{0, 0, float(kWidth), float(kHeight), 0, 1};
        VkRect2D sc{{0, 0}, {kWidth, kHeight}};
        VkPipelineViewportStateCreateInfo vps{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.pViewports = &vp;
        vps.scissorCount = 1;
        vps.pScissors = &sc;

        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;

        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb;
        gp.layout = pipeLayout;
        gp.renderPass = renderPass;
        gp.subpass = 0;
        VkResult pr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp,
            nullptr, &pipeline);
        if (pr != VK_SUCCESS)
        {
            lucent::warn("draw", "vkCreateGraphicsPipelines -> {}", VkStr(pr));
            return false;
        }
    }

    // --- descriptor pool + sets --------------------------------------------
    VkDescriptorPool pool = 0;
    {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 5},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2},
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = 4;
        ci.poolSizeCount = 4;
        ci.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(device, &ci, nullptr, &pool));
    }
    VkDescriptorSet sets[4] = {};
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 4;
        ai.pSetLayouts = setLayouts;
        VK_CHECK(vkAllocateDescriptorSets(device, &ai, sets));
    }

    auto bufInfo = [](VkBuffer b) {
        VkDescriptorBufferInfo i{};
        i.buffer = b;
        i.offset = 0;
        i.range = VK_WHOLE_SIZE;
        return i;
    };
    VkDescriptorBufferInfo biSsbo = bufInfo(ssbo);
    VkDescriptorBufferInfo biSys = bufInfo(uSys);
    VkDescriptorBufferInfo biFvs = bufInfo(uFvs);
    VkDescriptorBufferInfo biFps = bufInfo(uFps);
    VkDescriptorBufferInfo biBl = bufInfo(uBl);
    VkDescriptorBufferInfo biFetch = bufInfo(uFetch);
    VkDescriptorImageInfo iiTex{};
    iiTex.imageView = texView;
    iiTex.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo iiSamp{};
    iiSamp.sampler = samp;

    std::vector<VkWriteDescriptorSet> writes;
    auto wbuf = [&](VkDescriptorSet s, uint32_t b, VkDescriptorType t,
                    VkDescriptorBufferInfo* bi) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s; w.dstBinding = b; w.descriptorCount = 1;
        w.descriptorType = t; w.pBufferInfo = bi;
        writes.push_back(w);
    };
    auto wimg = [&](VkDescriptorSet s, uint32_t b, VkDescriptorType t,
                    VkDescriptorImageInfo* ii) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s; w.dstBinding = b; w.descriptorCount = 1;
        w.descriptorType = t; w.pImageInfo = ii;
        writes.push_back(w);
    };
    wbuf(sets[0], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &biSsbo);
    wbuf(sets[1], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biSys);
    wbuf(sets[1], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biFvs);
    wbuf(sets[1], 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biFps);
    wbuf(sets[1], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biBl);
    wbuf(sets[1], 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biFetch);
    wimg(sets[3], 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &iiTex);
    wimg(sets[3], 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &iiTex);
    wimg(sets[3], 2, VK_DESCRIPTOR_TYPE_SAMPLER, &iiSamp);
    vkUpdateDescriptorSets(device, uint32_t(writes.size()), writes.data(), 0, nullptr);

    // --- readback buffer ----------------------------------------------------
    const VkDeviceSize rbBytes = VkDeviceSize(kWidth) * kHeight * 4;
    VkBuffer readback = 0; VkDeviceMemory readbackMem = 0;
    if (!MakeBuffer(rbBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, readback, readbackMem))
        return false;

    // --- command buffer -----------------------------------------------------
    VkCommandPool cmdPool = 0;
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = queueFamily;
        VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &cmdPool));
    }
    VkCommandBuffer cmd = 0;
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
    }

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

    // The stub texture stands in for the render target the pixel shader samples,
    // which does not exist yet. Clear it to opaque white so the shader has a
    // defined, non-zero sample to work from -- that way the output colour is the
    // shader's actual computation over a known input, not undefined memory, and
    // a non-magenta, non-black frame proves the whole pipeline ran.
    {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = tex;
        toDst.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
        VkClearColorValue white{};
        white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &white, 1, &range);
        VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = tex;
        toRead.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
    }

    VkClearValue clear{};
    clear.color.float32[0] = 1.0f; // sentinel magenta: any non-magenta pixel is
    clear.color.float32[1] = 0.0f; // the rasterised quad, not the clear
    clear.color.float32[2] = 1.0f;
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rpb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpb.renderPass = renderPass;
    rpb.framebuffer = fb;
    rpb.renderArea = {{0, 0}, {kWidth, kHeight}};
    rpb.clearValueCount = 1;
    rpb.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout, 0,
        4, sets, 0, nullptr);
    vkCmdBindIndexBuffer(cmd, ibuf, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, in.indexCount, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);

    // Colour attachment is now in TRANSFER_SRC_OPTIMAL (render pass finalLayout).
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kWidth, kHeight, 1};
    vkCmdCopyImageToBuffer(cmd, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback, 1, &region);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkFence fence = 0;
    {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(device, &fi, nullptr, &fence));
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    // --- read pixels + screenshot ------------------------------------------
    g_frame.resize(rbBytes);
    {
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, readbackMem, 0, rbBytes, 0, &p));
        std::memcpy(g_frame.data(), p, rbBytes);
        vkUnmapMemory(device, readbackMem);
    }

    // Honest verification numbers: how much of the frame is NOT the sentinel
    // clear (i.e. actually shaded by the draw), and the centre pixel's colour.
    uint64_t nonClear = 0;
    for (uint32_t i = 0; i < kWidth * kHeight; ++i)
    {
        const uint8_t* px = &g_frame[size_t(i) * 4];
        if (!(px[0] == 255 && px[1] == 0 && px[2] == 255))
            ++nonClear;
    }
    const uint8_t* c = &g_frame[(size_t(kHeight / 2) * kWidth + kWidth / 2) * 4];
    lucent::info("draw", "rendered: {}/{} px written by the quad ({:.1f}%),"
        " centre pixel rgba=({},{},{},{})",
        nonClear, uint64_t(kWidth) * kHeight,
        100.0 * double(nonClear) / (double(kWidth) * kHeight),
        c[0], c[1], c[2], c[3]);
    if (nonClear == uint64_t(kWidth) * kHeight && c[0] == 0 && c[1] == 0 && c[2] == 0)
        lucent::info("draw", "the full-screen quad rasterised and the pixel shader"
            " ran over every pixel; output is black because this hot pair is an"
            " RT-sampling pass and texture0 is a 1x1 stub (the 1280x720 render"
            " target it samples is not produced yet)");

    const char* dir = std::getenv("GEARS_DRAW_DIR");
    std::filesystem::path out =
        (dir ? std::filesystem::path(dir) : std::filesystem::path("scratch/screenshots"))
        / "hot_draw.ppm";
    if (WritePpm(out, g_frame.data(), kWidth, kHeight))
        lucent::info("draw", "screenshot written to {}", out.string());
    else
        lucent::warn("draw", "could not write screenshot to {}", out.string());

    // --- teardown -----------------------------------------------------------
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyBuffer(device, readback, nullptr); vkFreeMemory(device, readbackMem, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipeLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, set0, nullptr);
    vkDestroyDescriptorSetLayout(device, set1, nullptr);
    vkDestroyDescriptorSetLayout(device, set2, nullptr);
    vkDestroyDescriptorSetLayout(device, set3, nullptr);
    vkDestroyFramebuffer(device, fb, nullptr);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyImageView(device, colorView, nullptr);
    vkDestroyImage(device, color, nullptr); vkFreeMemory(device, colorMem, nullptr);
    vkDestroySampler(device, samp, nullptr);
    vkDestroyImageView(device, texView, nullptr);
    vkDestroyImage(device, tex, nullptr); vkFreeMemory(device, texMem, nullptr);
    vkDestroyBuffer(device, ibuf, nullptr); vkFreeMemory(device, ibufMem, nullptr);
    vkDestroyBuffer(device, uFetch, nullptr); vkFreeMemory(device, mFetch, nullptr);
    vkDestroyBuffer(device, uBl, nullptr); vkFreeMemory(device, mBl, nullptr);
    vkDestroyBuffer(device, uFps, nullptr); vkFreeMemory(device, mFps, nullptr);
    vkDestroyBuffer(device, uFvs, nullptr); vkFreeMemory(device, mFvs, nullptr);
    vkDestroyBuffer(device, uSys, nullptr); vkFreeMemory(device, mSys, nullptr);
    vkDestroyBuffer(device, ssbo, nullptr); vkFreeMemory(device, ssboMem, nullptr);
    vkDestroyShaderModule(device, psMod, nullptr);
    vkDestroyShaderModule(device, vsMod, nullptr);
    return true;
}

// ---------------------------------------------------------------------------
// Whole-frame rendering. Every draw of the frame is issued, in submission
// order, into ONE persistent colour+depth target inside a single render pass so
// the geometry accumulates. Each draw carries its own register-file snapshot
// (constants live at that draw) and its own bound shader pair; distinct shader
// pairs are translated and their pipelines/modules cached across the frame.
bool Renderer::RenderFrameImpl(const FrameDrawInputs& in)
{
    const uint32_t W = in.width ? in.width : kWidth;
    const uint32_t H = in.height ? in.height : kHeight;

    // Phase timing. A whole-frame render costs ~390 ms today, which is two
    // orders of magnitude away from presenting live; before any of this is made
    // persistent it has to be clear WHICH phase the time is in, rather than
    // assuming it is the caches.
    using Clock = std::chrono::steady_clock;
    const auto tStart = Clock::now();
    auto sinceStartMs = [&] {
        return std::chrono::duration<double, std::milli>(Clock::now() - tStart).count();
    };
    double msSetup = 0, msDrawLoop = 0, msSubmit = 0, msReadback = 0;
    double msTranslate = 0, msPipeline = 0, msTexture = 0, msSsboUpload = 0;
    auto accumulate = [](double& into, Clock::time_point from) {
        into += std::chrono::duration<double, std::milli>(Clock::now() - from).count();
    };

    // Everything below that outlives a frame lives in P. It is built on the
    // first frame and reused after that; a change of target size rebuilds it,
    // since the render target and framebuffer are sized to the frame.
    if (persistent && (persistent->width != W || persistent->height != H))
        ReleasePersistent();
    if (!persistent)
    {
        persistent = new RendererPersistent();
        persistent->width = W;
        persistent->height = H;
    }
    RendererPersistent& P = *persistent;
    const bool firstFrame = !P.built;

    // --- translate + cache each distinct (shader, modification) ----------
    // The key is the PAIR (microcode hash, modification), not the hash alone:
    // the modification carries the interpolator mask the vertex and pixel
    // shaders exchange for THIS draw, so one microcode can legitimately need
    // several translations across a frame.
    using ShaderKey = std::pair<uint64_t, uint64_t>; // (hash, modification)
    std::map<ShaderKey, draw::ShaderXlate>& xlate = P.xlate;
    std::map<ShaderKey, VkShaderModule>& modules = P.modules;
    auto getShader = [&](bool isVertex, const uint8_t* uc, size_t sz, uint64_t hash,
                         uint64_t modification, draw::ShaderXlate*& outX,
                         VkShaderModule& outM) -> bool {
        const ShaderKey key{hash, modification};
        auto xit = xlate.find(key);
        if (xit == xlate.end())
        {
            draw::ShaderXlate x;
            const auto t0 = Clock::now();
            const bool translated =
                draw::TranslateShader(isVertex, uc, sz, hash, modification, x);
            accumulate(msTranslate, t0);
            if (!translated)
                return false;
            xit = xlate.emplace(key, std::move(x)).first;
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = xit->second.spirv.size();
            mi.pCode = reinterpret_cast<const uint32_t*>(xit->second.spirv.data());
            VkShaderModule m = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &mi, nullptr, &m) != VK_SUCCESS)
                return false;
            modules[key] = m;
        }
        outX = &xit->second;
        outM = modules[key];
        return true;
    };

    // --- shared SSBO: the guest physical memory the shaders fetch through ---
    // The buffer SPANS the whole guest physical window, because a vertex fetch
    // constant may name any address in it -- an Act 1 frame fetches as high as
    // 0xecf926c (237 MiB), and while this mirror was 64 MiB those fetches read
    // zero, collapsed every primitive to the origin and were destroyed at
    // clipping. That was the missing world (catalog #30).
    //
    // Spanning it does NOT mean copying it. The contents are uploaded per frame
    // (guest memory is precisely what changes between frames), and copying
    // 512 MiB per frame is not a renderer, it is a memcpy benchmark. So the
    // upload is DEFERRED to after the draw-preparation loop, which is where each
    // draw's vertex and index ranges become known, and only those ranges are
    // copied. See "deferred range upload" below.
    //
    // Deferring is safe because the SSBO's CONTENTS are only read by the GPU at
    // submit: the preparation loop reads indices from guestBase directly on the
    // CPU, and descriptor sets reference the buffer handle, not its bytes.
    if (P.ssbo != VK_NULL_HANDLE && P.ssboBytes != in.guestPhysicalMirrorBytes)
    {
        vkDestroyBuffer(device, P.ssbo, nullptr);
        vkFreeMemory(device, P.ssboMem, nullptr);
        P.ssbo = VK_NULL_HANDLE; P.ssboMem = VK_NULL_HANDLE;
    }
    if (P.ssbo == VK_NULL_HANDLE)
    {
        if (!MakeBuffer(in.guestPhysicalMirrorBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                P.ssbo, P.ssboMem))
            return false;
        P.ssboBytes = in.guestPhysicalMirrorBytes;
    }
    VkBuffer ssbo = P.ssbo;
    if (!P.ssboMapped)
        VK_CHECK(vkMapMemory(device, P.ssboMem, 0, in.guestPhysicalMirrorBytes, 0,
            &P.ssboMapped));
    // The ranges this frame's draws actually fetch, filled in by the preparation
    // loop and uploaded after it. Byte ranges into the guest window.
    std::vector<std::pair<uint64_t, uint64_t>> fetchRanges;

    // --- stub textures (1x1 white), one per image dimension --------------
    // A translated shader declares its image variables with the dimension the
    // guest's texture fetch used: 1D/2D become a 2D ARRAY image, k3DOrStacked a
    // 3D image, kCube a cube image (Xenia
    // SpirvShaderTranslator::FindOrAddTextureBinding). The descriptor written to
    // a binding must have the matching view type, so one stub of each kind is
    // created here and picked per binding. Real texture upload is the next step;
    // until then a sampling draw reads white rather than nothing.
    StubTex& stub2D = P.stub2D; StubTex& stub3D = P.stub3D; StubTex& stubCube = P.stubCube;
    VkSampler& samp = P.stubSampler;
    auto makeStub = [&](VkImageType imageType, VkImageViewType viewType,
                        uint32_t layers, uint32_t depth3d, VkImageCreateFlags flags,
                        StubTex& out) -> bool {
        VkImageCreateInfo ti{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ti.flags = flags;
        ti.imageType = imageType;
        ti.format = VK_FORMAT_R8G8B8A8_UNORM;
        ti.extent = {1, 1, depth3d};
        ti.mipLevels = 1;
        ti.arrayLayers = layers;
        ti.samples = VK_SAMPLE_COUNT_1_BIT;
        ti.tiling = VK_IMAGE_TILING_OPTIMAL;
        ti.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateImage(device, &ti, nullptr, &out.image));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, out.image, &req);
        uint32_t type = 0;
        if (!FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
            FindMemory(req.memoryTypeBits, 0, type);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &out.mem));
        VK_CHECK(vkBindImageMemory(device, out.image, out.mem, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.image;
        vi.viewType = viewType;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        VK_CHECK(vkCreateImageView(device, &vi, nullptr, &out.view));
        return true;
    };
    if (firstFrame &&
        (!makeStub(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 1, 0, stub2D) ||
         !makeStub(VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 1, 0, stub3D) ||
         !makeStub(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, 1,
                   VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, stubCube)))
        return false;
    if (firstFrame)
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(device, &si, nullptr, &samp));
    }

    // --- guest texture upload --------------------------------------------
    // Every texture the frame samples is described by its own texture fetch
    // constant. gpu_draw_xlate decodes one (Xenia's texture_util /
    // texture_address / FormatInfo do the layout, detiling and format
    // classification); here it becomes a host image. A fetch whose format has
    // no host mapping keeps the stub AND is counted with its reason -- nothing
    // is ever substituted to make the frame look better.
    std::vector<GuestTex>& guestTextures = P.guestTextures;
    std::map<std::array<uint32_t, 4>, VkImageView>& texCache = P.texCache;
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

    auto hostVkFormat = [](draw::TexHostFormat f) -> VkFormat {
        switch (f)
        {
            case draw::TexHostFormat::kR8Unorm: return VK_FORMAT_R8_UNORM;
            case draw::TexHostFormat::kR8G8Unorm: return VK_FORMAT_R8G8_UNORM;
            case draw::TexHostFormat::kR8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
            case draw::TexHostFormat::kR5G6B5Pack16: return VK_FORMAT_B5G6R5_UNORM_PACK16;
            case draw::TexHostFormat::kA1R5G5B5Pack16: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
            case draw::TexHostFormat::kB4G4R4A4Pack16: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
            case draw::TexHostFormat::kA2B10G10R10Pack32:
                return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case draw::TexHostFormat::kR16Sfloat: return VK_FORMAT_R16_SFLOAT;
            case draw::TexHostFormat::kR16G16Sfloat: return VK_FORMAT_R16G16_SFLOAT;
            case draw::TexHostFormat::kR16G16B16A16Sfloat:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case draw::TexHostFormat::kR16Unorm: return VK_FORMAT_R16_UNORM;
            case draw::TexHostFormat::kR16G16Unorm: return VK_FORMAT_R16G16_UNORM;
            case draw::TexHostFormat::kR16G16B16A16Unorm:
                return VK_FORMAT_R16G16B16A16_UNORM;
            case draw::TexHostFormat::kR32Sfloat: return VK_FORMAT_R32_SFLOAT;
            case draw::TexHostFormat::kR32G32Sfloat: return VK_FORMAT_R32G32_SFLOAT;
            case draw::TexHostFormat::kR32G32B32A32Sfloat:
                return VK_FORMAT_R32G32B32A32_SFLOAT;
            case draw::TexHostFormat::kBc1RgbaUnorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case draw::TexHostFormat::kBc2Unorm: return VK_FORMAT_BC2_UNORM_BLOCK;
            case draw::TexHostFormat::kBc3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
            case draw::TexHostFormat::kBc4Unorm: return VK_FORMAT_BC4_UNORM_BLOCK;
            case draw::TexHostFormat::kBc5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
            default: return VK_FORMAT_UNDEFINED;
        }
    };
    auto compSwizzle = [](uint8_t s) -> VkComponentSwizzle {
        switch (s)
        {
            case 0: return VK_COMPONENT_SWIZZLE_R;
            case 1: return VK_COMPONENT_SWIZZLE_G;
            case 2: return VK_COMPONENT_SWIZZLE_B;
            case 3: return VK_COMPONENT_SWIZZLE_A;
            case 4: return VK_COMPONENT_SWIZZLE_ZERO;
            default: return VK_COMPONENT_SWIZZLE_ONE;
        }
    };

    // Builds (once per distinct fetch) the host image for one texture fetch
    // constant, or returns VK_NULL_HANDLE with the reason counted.
    auto uploadTexture = [&](const uint32_t* fetch6, uint32_t wantDim) -> VkImageView {
        const std::array<uint32_t, 4> key{fetch6[0], fetch6[1], fetch6[2],
                                          fetch6[3] & 0x1FFEu /*swizzle bits*/};
        auto it = texCache.find(key);
        if (it != texCache.end())
            return it->second;
        texDistinct.insert(key);

        draw::GuestTexture gt;
        if (!draw::DecodeGuestTexture(fetch6, in.guestBase,
                uint64_t(in.guestWindowBytes), /*wantData=*/true, gt))
        {
            ++texSkips["not a texture fetch constant"];
            texCache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        ++texFormatBindings[gt.formatName];
        {
            std::string s = std::format(
                "{:#x} {} {}x{}x{} dim{} {} endian{} swizzle{:#05x} mips{}-{}{}",
                gt.baseAddress, gt.formatName, gt.width, gt.height,
                gt.depthOrArraySize, gt.dimension, gt.tiled ? "tiled" : "linear",
                gt.endian, gt.guestSwizzle, gt.mipMin, gt.mipMax,
                gt.packedMips ? " packed" : "");
            ++texFormatCensus[s];
        }
        if (gt.skipReason)
        {
            ++texSkips[gt.skipReason];
            texCache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        // The shader's declared image type has to match the view type, and the
        // shader derived it from this same fetch's dimension -- a mismatch
        // means our decode disagrees with the translator, which we report
        // rather than paper over.
        const uint32_t declDim = gt.dimension <= 1 ? 1 : gt.dimension;
        if ((wantDim <= 1 ? 1u : wantDim) != declDim)
        {
            ++texSkips["shader/fetch dimension mismatch"];
            texCache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        const VkFormat vf = hostVkFormat(gt.hostFormat);
        if (vf == VK_FORMAT_UNDEFINED)
        {
            ++texSkips["no host VkFormat"];
            texCache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(physical, vf, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        {
            ++texSkips["host format not sampleable on this device"];
            texCache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }

        const bool is3D = gt.dimension == 2;
        const bool isCube = gt.dimension == 3;
        GuestTex tex;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = isCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ci.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ci.format = vf;
        ci.extent = {gt.width, gt.height, gt.depth3D};
        ci.mipLevels = 1;
        ci.arrayLayers = gt.layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ci, nullptr, &tex.image) != VK_SUCCESS)
        {
            ++texSkips["vkCreateImage failed"];
            texCache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, tex.image, &req);
        uint32_t mtype = 0;
        if (!FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, mtype))
            FindMemory(req.memoryTypeBits, 0, mtype);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mtype;
        if (vkAllocateMemory(device, &ai, nullptr, &tex.mem) != VK_SUCCESS ||
            vkBindImageMemory(device, tex.image, tex.mem, 0) != VK_SUCCESS)
        {
            vkDestroyImage(device, tex.image, nullptr);
            ++texSkips["image memory allocation failed"];
            texCache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = tex.image;
        vi.viewType = is3D ? VK_IMAGE_VIEW_TYPE_3D
                   : isCube ? VK_IMAGE_VIEW_TYPE_CUBE
                            : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format = vf;
        vi.components.r = compSwizzle(gt.hostSwizzle[0]);
        vi.components.g = compSwizzle(gt.hostSwizzle[1]);
        vi.components.b = compSwizzle(gt.hostSwizzle[2]);
        vi.components.a = compSwizzle(gt.hostSwizzle[3]);
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, gt.layers};
        if (vkCreateImageView(device, &vi, nullptr, &tex.view) != VK_SUCCESS)
        {
            vkDestroyImage(device, tex.image, nullptr);
            vkFreeMemory(device, tex.mem, nullptr);
            ++texSkips["vkCreateImageView failed"];
            texCache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }

        VkBuffer staging = 0; VkDeviceMemory stagingMem = 0;
        if (!MakeBuffer(gt.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        staging, stagingMem))
        {
            vkDestroyImageView(device, tex.view, nullptr);
            vkDestroyImage(device, tex.image, nullptr);
            vkFreeMemory(device, tex.mem, nullptr);
            ++texSkips["staging buffer allocation failed"];
            texCache[key] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        {
            void* p = nullptr;
            vkMapMemory(device, stagingMem, 0, gt.data.size(), 0, &p);
            std::memcpy(p, gt.data.data(), gt.data.size());
            vkUnmapMemory(device, stagingMem);
        }
        // GEARS_DRAW_TEX_DUMP=1 writes the decoded (detiled, endian-swapped)
        // blob so the decode can be checked outside the renderer -- the only
        // way to tell "detiling is right" from "the shader is dark".
        if (std::getenv("GEARS_DRAW_TEX_DUMP"))
        {
            std::filesystem::create_directories("scratch/raw/textures");
            const std::string fn = std::format(
                "scratch/raw/textures/{:08x}_{}_{}x{}x{}_{}.bin", gt.baseAddress,
                gt.formatName, gt.width, gt.height, gt.layers * gt.depth3D,
                gt.tiled ? "tiled" : "linear");
            if (FILE* f = std::fopen(fn.c_str(), "wb"))
            {
                std::fwrite(gt.data.data(), 1, gt.data.size(), f);
                std::fclose(f);
            }
        }
        stagingBufs.push_back(staging);
        stagingMems.push_back(stagingMem);
        uploadedBytes += gt.data.size();
        uploads.push_back({tex.image, staging, gt.width, gt.height, gt.depth3D,
                           gt.layers});
        guestTextures.push_back(tex);
        texCache[key] = tex.view;
        return tex.view;
    };

    // Sampler per distinct guest sampler state, built from the shader's sampler
    // binding resolved against its own fetch constant (clamp modes, filters,
    // anisotropy) -- not a fixed host sampler.
    std::map<uint64_t, VkSampler>& samplerCache = P.samplerCache;
    auto vkAddressMode = [](uint32_t clamp) -> VkSamplerAddressMode {
        switch (clamp)
        {
            case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            // Half-way clamps have no host equivalent; edge is the closest and
            // is recorded as such rather than silently pretended to be exact.
            case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case 5: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
            case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
    };
    auto getSampler = [&](const draw::GuestSamplerState& gs) -> VkSampler {
        const uint64_t k = uint64_t(gs.magFilter) | (uint64_t(gs.minFilter) << 4) |
                           (uint64_t(gs.mipFilter) << 8) |
                           (uint64_t(gs.clamp[0]) << 12) | (uint64_t(gs.clamp[1]) << 16) |
                           (uint64_t(gs.clamp[2]) << 20) | (uint64_t(gs.anisoMax) << 24);
        auto it = samplerCache.find(k);
        if (it != samplerCache.end())
            return it->second;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = gs.magFilter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.minFilter = gs.minFilter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = gs.mipFilter == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                          : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = vkAddressMode(gs.clamp[0]);
        si.addressModeV = vkAddressMode(gs.clamp[1]);
        si.addressModeW = vkAddressMode(gs.clamp[2]);
        si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        si.maxLod = VK_LOD_CLAMP_NONE;
        VkSampler s = VK_NULL_HANDLE;
        if (vkCreateSampler(device, &si, nullptr, &s) != VK_SUCCESS)
            return samp;
        samplerCache[k] = s;
        return s;
    };

    // --- shared depth ------------------------------------------------------
    VkImage& depth = P.depth; VkDeviceMemory& depthMem = P.depthMem;
    VkImageView& depthView = P.depthView;
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    if (firstFrame)
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = depthFormat;
        ci.extent = {W, H, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        VK_CHECK(vkCreateImage(device, &ci, nullptr, &depth));
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, depth, &req);
        uint32_t type = 0;
        if (!FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
            FindMemory(req.memoryTypeBits, 0, type);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device, &ai, nullptr, &depthMem));
        VK_CHECK(vkBindImageMemory(device, depth, depthMem, 0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = depth;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = depthFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device, &vi, nullptr, &depthView));

        VkImageCreateInfo si{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        si.imageType = VK_IMAGE_TYPE_2D;
        si.format = VK_FORMAT_R8G8B8A8_UNORM;
        si.extent = {W, H, 1};
        si.mipLevels = 1;
        si.arrayLayers = 1;
        si.samples = VK_SAMPLE_COUNT_1_BIT;
        si.tiling = VK_IMAGE_TILING_OPTIMAL;
        si.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateImage(device, &si, nullptr, &P.presentStage));
        VkMemoryRequirements sreq{};
        vkGetImageMemoryRequirements(device, P.presentStage, &sreq);
        uint32_t stype = 0;
        if (!FindMemory(sreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, stype))
            FindMemory(sreq.memoryTypeBits, 0, stype);
        VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        sai.allocationSize = sreq.size;
        sai.memoryTypeIndex = stype;
        VK_CHECK(vkAllocateMemory(device, &sai, nullptr, &P.presentStageMem));
        VK_CHECK(vkBindImageMemory(device, P.presentStage, P.presentStageMem, 0));
    }
    VkImage presentStage = P.presentStage;

    // --- render pass (colour + depth) ------------------------------------
    // Two variants of the same pass per host colour format: one that CLEARS the
    // surface (its first use in a frame) and one that LOADS it (every use
    // after, so the console's predicated tiles accumulate). The frame is also
    // split at every surface change and at every resolve, because a surface can
    // only be blitted out while no pass is open on it.
    auto makeRenderPass = [&](VkFormat colorFormat, bool load, VkRenderPass& out) {
        VkAttachmentDescription att[2]{};
        att[0].format = colorFormat;
        att[0].samples = VK_SAMPLE_COUNT_1_BIT;
        att[0].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[0].initialLayout = load ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                    : VK_IMAGE_LAYOUT_UNDEFINED;
        att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        att[1].format = depthFormat;
        att[1].samples = VK_SAMPLE_COUNT_1_BIT;
        att[1].loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[1].initialLayout = load ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                    : VK_IMAGE_LAYOUT_UNDEFINED;
        att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        sub.pDepthStencilAttachment = &dref;
        VkSubpassDependency deps[2]{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = 0;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 2;
        rp.pAttachments = att;
        rp.subpassCount = 1;
        rp.pSubpasses = &sub;
        rp.dependencyCount = 2;
        rp.pDependencies = deps;
        VK_CHECK(vkCreateRenderPass(device, &rp, nullptr, &out));
        return true;
    };
    // One (clear, load) pair per host colour format, created on first use.
    auto getPasses = [&](VkFormat colorFormat,
                         std::pair<VkRenderPass, VkRenderPass>*& out) -> bool {
        auto it = P.passes.find(colorFormat);
        if (it == P.passes.end())
        {
            std::pair<VkRenderPass, VkRenderPass> rp{VK_NULL_HANDLE, VK_NULL_HANDLE};
            if (!makeRenderPass(colorFormat, false, rp.first) ||
                !makeRenderPass(colorFormat, true, rp.second))
                return false;
            it = P.passes.emplace(colorFormat, rp).first;
        }
        out = &it->second;
        return true;
    };

    // Every RB_COLOR_INFO.color_format the frame renders each EDRAM base with,
    // filled by the pre-pass further down and read by getSurfaceTarget, which
    // sizes one host image per base wide enough to hold all of them.
    std::map<uint32_t, std::set<uint32_t>> formatsPerBase;

    // The render-target cache proper: a host colour target per EDRAM surface,
    // created the first time a frame renders into that (base, format) and kept
    // for the life of the run like every other persistent object here.
    auto getSurfaceTarget = [&](uint32_t base, SurfaceTarget*& out) -> bool {
        auto it = P.surfaceTargets.find(base);
        if (it != P.surfaceTargets.end()) { out = &it->second; return true; }
        auto fmts = formatsPerBase.find(base);
        if (fmts == formatsPerBase.end())
            return false;
        bool mixed = false;
        const VkFormat hostFormat = HostFormatFor(fmts->second, mixed);
        if (hostFormat == VK_FORMAT_UNDEFINED)
            return false;
        std::pair<VkRenderPass, VkRenderPass>* rp = nullptr;
        if (!getPasses(hostFormat, rp))
            return false;
        SurfaceTarget s;
        s.hostFormat = hostFormat;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = hostFormat;
        ci.extent = {W, H, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC so a resolve can copy it out and the presented surface
        // can be read back.
        ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateImage(device, &ci, nullptr, &s.color) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, s.color, &req);
        uint32_t type = 0;
        if (!FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
            FindMemory(req.memoryTypeBits, 0, type);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &ai, nullptr, &s.colorMem) != VK_SUCCESS ||
            vkBindImageMemory(device, s.color, s.colorMem, 0) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = s.color;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = hostFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &s.colorView) != VK_SUCCESS)
            return false;
        VkImageView atts[2] = {s.colorView, depthView};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp->first;
        fi.attachmentCount = 2;
        fi.pAttachments = atts;
        fi.width = W;
        fi.height = H;
        fi.layers = 1;
        if (vkCreateFramebuffer(device, &fi, nullptr, &s.fb) != VK_SUCCESS)
            return false;
        {
            lucent::Line nl;
            nl.add("render-target cache: new surface {:#x} ->", base);
            for (uint32_t f : fmts->second)
                nl.add(" {}", ColorFormatName(f));
            nl.add(" in one host target {}x{}{}", W, H,
                   mixed ? " (reinterpreted mid-frame; widened host format)" : "");
            nl.flush(lucent::Level::Info, "draw");
        }
        out = &P.surfaceTargets.emplace(base, s).first->second;
        return true;
    };

    // The host image a resolve destination owns.
    //
    // It is always R16G16B16A16_SFLOAT, whatever its source surface's format
    // is, and the resolve is a BLIT rather than a copy. That is not laziness:
    // measured on an Act 1 frame, destination 0xbde0000 receives resolves from
    // BOTH the 8888 tonemap surface at 0x2d0 and the 7e3 HDR surface at 0x400,
    // so no single source format describes it. A wide float destination holds
    // an 8888 UNORM value (0..1) exactly and a 7e3 HDR value (0..32) without
    // clamping, and vkCmdBlitImage does the conversion; an 8888 destination
    // would clamp every HDR highlight the tonemap pass exists to read.
    auto getResolveTarget = [&](uint32_t destBase, uint32_t sourceBase,
                                ResolveTarget*& out) -> bool {
        auto it = P.resolveTargets.find(destBase);
        if (it != P.resolveTargets.end()) { out = &it->second; return true; }
        const VkFormat hostFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        ResolveTarget r;
        r.hostFormat = hostFormat;
        r.sourceBase = sourceBase;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = hostFormat;
        ci.extent = {W, H, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(device, &ci, nullptr, &r.image) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, r.image, &req);
        uint32_t type = 0;
        if (!FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type))
            FindMemory(req.memoryTypeBits, 0, type);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &ai, nullptr, &r.mem) != VK_SUCCESS ||
            vkBindImageMemory(device, r.image, r.mem, 0) != VK_SUCCESS)
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = r.image;
        // 2D_ARRAY because the translated shaders declare their 2D textures as
        // arrays, exactly as the stub and guest texture views do.
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vi.format = hostFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &r.view) != VK_SUCCESS)
            return false;
        lucent::info("draw", "render-target cache: resolve destination {:#x} <- surface"
            " {:#x}", destBase, sourceBase);
        out = &P.resolveTargets.emplace(destBase, r).first->second;
        return true;
    };

    // --- descriptor set layouts (same as the hot-draw path) --------------
    auto makeSetLayout = [&](const std::vector<VkDescriptorSetLayoutBinding>& b,
                             VkDescriptorSetLayout& l) -> bool {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(b.size());
        ci.pBindings = b.empty() ? nullptr : b.data();
        VK_CHECK(vkCreateDescriptorSetLayout(device, &ci, nullptr, &l));
        return true;
    };
    const VkShaderStageFlags allStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayout& set0 = P.set0;
    VkDescriptorSetLayout& set1 = P.set1;
    if (firstFrame &&
        (!makeSetLayout({{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, allStages, nullptr}}, set0) ||
        !makeSetLayout({
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
            {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
            {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr},
            {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, allStages, nullptr}}, set1)))
        return false;

    // --- per-shader texture descriptor set layouts (sets 2 and 3) --------
    // Sets 2/3 are Xenia's kDescriptorSetTexturesVertex/Pixel: their contents
    // are decided by the SHADER (N images at bindings 0..N-1, then M samplers at
    // bindings N..N+M-1), so one fixed layout cannot serve every draw. Build a
    // layout per distinct (image dimensions, sampler count) signature, cached.
    // Getting this wrong is not a validation warning -- it is undefined
    // behaviour that crashed the RADV compiler inside lower_immediate_samplers.
    auto texSignature = [](const draw::ShaderXlate& x, VkShaderStageFlags stage) {
        std::string s;
        s.reserve(x.textures.size() * 2 + 8);
        for (const auto& t : x.textures)
            s.push_back(char('0' + (t.dimension & 3)));
        s.push_back('|');
        s += std::to_string(x.samplerCount);
        s.push_back('|');
        s += std::to_string(stage);
        return s;
    };
    std::map<std::string, VkDescriptorSetLayout>& texLayouts = P.texLayouts;
    auto getTexLayout = [&](const draw::ShaderXlate& x, VkShaderStageFlags stage,
                            VkDescriptorSetLayout& out) -> bool {
        const std::string key = texSignature(x, stage);
        auto it = texLayouts.find(key);
        if (it != texLayouts.end()) { out = it->second; return true; }
        std::vector<VkDescriptorSetLayoutBinding> b;
        for (uint32_t i = 0; i < uint32_t(x.textures.size()); ++i)
            b.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stage, nullptr});
        for (uint32_t j = 0; j < x.samplerCount; ++j)
            b.push_back({uint32_t(x.textures.size()) + j, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                         stage, nullptr});
        VkDescriptorSetLayout l = 0;
        if (!makeSetLayout(b, l))
            return false;
        texLayouts[key] = l;
        out = l;
        return true;
    };

    // A pipeline layout per (vertex texture signature, pixel texture signature).
    std::map<std::pair<std::string, std::string>, VkPipelineLayout>& pipeLayouts =
        P.pipeLayouts;
    auto getPipeLayout = [&](const draw::ShaderXlate& vsX, const draw::ShaderXlate& psX,
                             VkDescriptorSetLayout& outVsTex,
                             VkDescriptorSetLayout& outPsTex,
                             VkPipelineLayout& out) -> bool {
        if (!getTexLayout(vsX, VK_SHADER_STAGE_VERTEX_BIT, outVsTex) ||
            !getTexLayout(psX, VK_SHADER_STAGE_FRAGMENT_BIT, outPsTex))
            return false;
        auto key = std::make_pair(texSignature(vsX, VK_SHADER_STAGE_VERTEX_BIT),
                                  texSignature(psX, VK_SHADER_STAGE_FRAGMENT_BIT));
        auto it = pipeLayouts.find(key);
        if (it != pipeLayouts.end()) { out = it->second; return true; }
        VkDescriptorSetLayout sets[4] = {set0, set1, outVsTex, outPsTex};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 4;
        pi.pSetLayouts = sets;
        VkPipelineLayout pl = 0;
        if (vkCreatePipelineLayout(device, &pi, nullptr, &pl) != VK_SUCCESS)
            return false;
        pipeLayouts[key] = pl;
        out = pl;
        return true;
    };

    // --- rectangle-list geometry shaders, cached by their derived shape ---
    // A rectangle list carries three vertices per rectangle and the hardware
    // infers the fourth by mirroring one across the longest edge. The fourth
    // vertex's ATTRIBUTES are mirrored the same way, so it cannot be synthesized
    // in the index buffer ahead of the vertex shader -- the expansion has to see
    // shaded vertices. draw::BuildRectangleGeometryShader is the port of the
    // shader Xenia uses for exactly this.
    std::map<draw::RectangleGeometryShaderKey, VkShaderModule>& geomShaders =
        P.geomShaders;
    uint32_t rectDraws = 0, rectDrawsExpanded = 0;
    auto getRectGeomShader = [&](uint64_t vsModification, VkShaderModule& out) -> bool {
        out = VK_NULL_HANDLE;
        if (!hasGeometryShader)
            return false;
        draw::RectangleGeometryShaderKey key;
        if (!draw::DeriveRectangleGeometryShaderKey(vsModification, key))
            return false;
        auto it = geomShaders.find(key);
        if (it != geomShaders.end())
        { out = it->second; return out != VK_NULL_HANDLE; }
        std::vector<uint32_t> spirv;
        VkShaderModule mod = VK_NULL_HANDLE;
        if (draw::BuildRectangleGeometryShader(key, spirv))
        {
            VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            ci.codeSize = spirv.size() * sizeof(uint32_t);
            ci.pCode = spirv.data();
            if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS)
                mod = VK_NULL_HANDLE;
            else
                lucent::info("draw", "rectangle geometry shader: {} interpolators,"
                    " {} clip, {} cull distances, {} SPIR-V words",
                    key.interpolatorCount, key.clipDistanceCount,
                    key.cullDistanceCount, spirv.size());
        }
        if (mod == VK_NULL_HANDLE)
            lucent::warn("draw", "rectangle geometry shader build failed");
        geomShaders[key] = mod;
        out = mod;
        return mod != VK_NULL_HANDLE;
    };

    // --- pipeline cache keyed on (vs,ps,gs,prim,output-merger state) ------
    // Keyed on the MODULE HANDLES, not the microcode hashes: one microcode now
    // translates to several distinct shaders (one per modification), so a hash
    // does not identify a stage.
    auto& pipelines = P.pipelines;
    auto getPipeline = [&](VkShaderModule vsMod, VkShaderModule psMod,
                           VkShaderModule gsMod, uint32_t primType,
                           const OutputMergerState& om, VkRenderPass renderPass,
                           VkPipelineLayout pipeLayout, VkPipeline& out) -> bool {
        auto key = std::make_tuple(vsMod, psMod, gsMod, primType, om, renderPass);
        auto it = pipelines.find(key);
        if (it != pipelines.end()) { out = it->second; return true; }
        VkPipelineShaderStageCreateInfo stages[3]{};
        uint32_t stageCount = 0;
        stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[stageCount].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[stageCount].module = vsMod; stages[stageCount].pName = "main";
        ++stageCount;
        if (gsMod != VK_NULL_HANDLE)
        {
            stages[stageCount] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[stageCount].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
            stages[stageCount].module = gsMod; stages[stageCount].pName = "main";
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
            stages[stageCount].module = psMod; stages[stageCount].pName = "main";
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
        VkPipelineViewportStateCreateInfo vps{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dynStates;
        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
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
        static const bool noDepth = std::getenv("GEARS_DRAW_NODEPTH") != nullptr;
        ds.depthTestEnable =
            (!noDepth && ((om.depthControl >> 1) & 1)) ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = ((om.depthControl >> 2) & 1) ? VK_TRUE : VK_FALSE;
        ds.depthCompareOp = CompareOpOf(om.depthControl >> 4);
        // Colour write mask from RB_COLOR_MASK's RT0 nibble (r,g,b,a in bits
        // 0..3), and blending from RB_BLENDCONTROL0. A draw the guest masked off
        // entirely writes nothing, as on hardware.
        VkPipelineColorBlendAttachmentState cba{};
        if (om.colorMask & 1) cba.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
        if (om.colorMask & 2) cba.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
        if (om.colorMask & 4) cba.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
        if (om.colorMask & 8) cba.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
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
        static const bool noBlend = std::getenv("GEARS_DRAW_NOBLEND") != nullptr;
        cba.blendEnable = (noBlend || blendIsIdentity) ? VK_FALSE : VK_TRUE;
        cba.srcColorBlendFactor = BlendFactorOf(cSrc);
        cba.dstColorBlendFactor = BlendFactorOf(cDst);
        cba.colorBlendOp = BlendOpOf(cOp);
        cba.srcAlphaBlendFactor = BlendFactorOf(aSrc);
        cba.dstAlphaBlendFactor = BlendFactorOf(aDst);
        cba.alphaBlendOp = BlendOpOf(aOp);
        VkPipelineColorBlendStateCreateInfo cb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = stageCount; gp.pStages = stages;
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
            vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe);
        accumulate(msPipeline, tPipe);
        if (pipeResult != VK_SUCCESS)
            return false;
        pipelines[key] = pipe;
        out = pipe;
        return true;
    };

    // --- descriptor pool sized for every draw ----------------------------
    msSetup = sinceStartMs();
    const uint32_t nDraws = uint32_t(in.draws.size());
    VkDescriptorPool& pool = P.descriptorPool;
    if (pool != VK_NULL_HANDLE && P.descriptorPoolDraws < nDraws)
    {
        vkDestroyDescriptorPool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
    }
    if (pool != VK_NULL_HANDLE)
    {
        VK_CHECK(vkResetDescriptorPool(device, pool, 0));
    }
    else
    {
        // Image/sampler counts are per shader (up to 32 texture fetch constants
        // per stage on Xenos), so size for the worst case rather than the two
        // the loading frame happened to use.
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, std::max(nDraws, 1u)},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, std::max(nDraws * 5, 1u)},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, std::max(nDraws * 64, 1u)},
            {VK_DESCRIPTOR_TYPE_SAMPLER, std::max(nDraws * 64, 1u)}};
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = std::max(nDraws * 4, 4u);
        ci.poolSizeCount = 4;
        ci.pPoolSizes = sizes;
        VK_CHECK(vkCreateDescriptorPool(device, &ci, nullptr, &pool));
        P.descriptorPoolDraws = nDraws;
    }

    // Per-draw resources, kept alive until after the submit completes. The
    // fallback path only runs when a frame outgrows the arena.
    std::vector<VkBuffer> keepBuffers;
    std::vector<VkDeviceMemory> keepMem;

    // Size the arena to the previous frame's high-water mark before the frame
    // starts, so the common case never allocates. The first frame has no mark
    // to go on and estimates from the draw count; if the estimate is short the
    // overflow path covers the rest and the real mark sizes the next frame.
    if (P.arenaHighWater == 0)
        P.arenaHighWater = VkDeviceSize(nDraws) * 16384;
    if (P.arenaHighWater > P.arenaBytes)
    {
        if (P.arenaMapped)
            vkUnmapMemory(device, P.arenaMem);
        vkDestroyBuffer(device, P.arena, nullptr);
        vkFreeMemory(device, P.arenaMem, nullptr);
        P.arena = VK_NULL_HANDLE; P.arenaMem = VK_NULL_HANDLE; P.arenaMapped = nullptr;
        const VkDeviceSize want = P.arenaHighWater + P.arenaHighWater / 4 + 0x10000;
        if (MakeBuffer(want, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT, P.arena, P.arenaMem))
        {
            P.arenaBytes = want;
            VK_CHECK(vkMapMemory(device, P.arenaMem, 0, want, 0, &P.arenaMapped));
            lucent::info("draw", "per-draw arena grown to {} KiB", want / 1024);
        }
        else
        {
            P.arenaBytes = 0;
        }
    }
    VkDeviceSize arenaCursor = 0;
    uint32_t arenaOverflows = 0;

    // Copies `size` bytes into the arena and reports where they landed, or
    // returns false when the arena is full (the caller then falls back).
    auto arenaWrite = [&](const void* data, size_t size, VkDeviceSize alignment,
                          VkDeviceSize& outOffset) -> bool {
        if (!P.arenaMapped)
            return false;
        const VkDeviceSize offset = (arenaCursor + alignment - 1) & ~(alignment - 1);
        if (offset + size > P.arenaBytes)
            return false;
        std::memcpy(static_cast<uint8_t*>(P.arenaMapped) + offset, data, size);
        arenaCursor = offset + size;
        outOffset = offset;
        return true;
    };

    // An index buffer for one draw, from the same arena. Index offsets need
    // 4-byte alignment; the buffer is bound with that offset.
    auto makeIndexBuffer = [&](const void* data, size_t bytes, VkBuffer& outBuf,
                               VkDeviceSize& outOffset) -> bool {
        if (arenaWrite(data, bytes, 4, outOffset))
        {
            outBuf = P.arena;
            return true;
        }
        ++arenaOverflows;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (!MakeBuffer(bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, outBuf, m))
            return false;
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, m, 0, bytes, 0, &p));
        std::memcpy(p, data, bytes);
        vkUnmapMemory(device, m);
        keepBuffers.push_back(outBuf); keepMem.push_back(m);
        outOffset = 0;
        return true;
    };

    // A uniform block for one draw: (buffer, offset, range) rather than a whole
    // VkBuffer of its own.
    auto makeUbo = [&](const void* data, size_t size,
                       VkDescriptorBufferInfo& out) -> bool {
        const size_t bytes = std::max<size_t>(size, 16);
        VkDeviceSize offset = 0;
        if (arenaWrite(data, bytes, uniformOffsetAlignment, offset))
        {
            out = {P.arena, offset, bytes};
            return true;
        }
        ++arenaOverflows;
        VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE;
        if (!MakeBuffer(bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, b, m))
            return false;
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, m, 0, bytes, 0, &p));
        std::memcpy(p, data, size);
        vkUnmapMemory(device, m);
        keepBuffers.push_back(b); keepMem.push_back(m);
        out = {b, 0, bytes};
        return true;
    };

    struct PreparedDraw
    {
        VkPipeline pipeline;
        VkPipelineLayout layout;
        VkDescriptorSet sets[4];
        VkBuffer ibuf;       // VK_NULL_HANDLE for a non-indexed (auto) draw
        VkDeviceSize ibufOffset; // where in the arena this draw's indices live
        uint32_t count;      // index count, or vertex count when !indexed
        bool indexed;
        VkViewport viewport; // the guest's own, per draw
        VkRect2D scissor;
        // Which EDRAM surface this draw renders into. The recording pass below
        // walks these in submission order and re-binds the render pass whenever
        // the surface changes.
        uint32_t surfaceBase = 0;
        // A resolve (RB_MODECONTROL.edram_mode == kCopy) is not geometry: it
        // copies `surface`'s host target out to `resolveDest`'s host image, and
        // issues no draw at all.
        bool isResolve = false;
        uint32_t resolveDest = 0;
        // --- diagnostic only (GEARS_DRAW_DIAG), never read by the renderer ---
        // Everything that can make a draw contribute nothing, recorded next to
        // the draw that did nothing. A summary cannot answer "which stage did
        // this surface's draws die at" -- only the join of per-draw state with
        // per-draw pipeline statistics can, and that join is this table.
        uint32_t diagIndex = 0;      // index in the frame's submission order
        uint32_t edramMode = 0;
        uint32_t primType = 0;
        uint64_t vsHash = 0, psHash = 0;
        bool hasFragmentStage = false;
        uint32_t colorMask = 0, depthControl = 0, blend0 = 0;
        uint32_t colorFormat = 0;    // RB_COLOR_INFO color_format
        // The state that decides whether a primitive survives to rasterisation,
        // which is where this frame's world geometry dies. Raw, so the table
        // shows what the guest programmed rather than our interpretation of it.
        uint32_t clipCntl = 0;       // PA_CL_CLIP_CNTL   0x2204
        uint32_t suScModeCntl = 0;   // PA_SU_SC_MODE_CNTL 0x2205
        uint32_t vteCntl = 0;        // PA_CL_VTE_CNTL    0x206C
        uint32_t windowOffset = 0;   // PA_SC_WINDOW_OFFSET 0x2080
        float vportXScale = 0, vportXOffset = 0, vportYScale = 0, vportYOffset = 0;
        float vportZScale = 0, vportZOffset = 0;
    };
    std::vector<PreparedDraw> prepared;
    uint32_t issued = 0, skipped = 0;

    // Per-EDRAM-surface census. A draw's RB_COLOR_INFO (0x2001) names the EDRAM
    // tile base it renders into; distinct bases are distinct surfaces (scene
    // colour, light attenuation, shadow depth, the post chain...). UE3-on-360
    // renders each surface in predicated TILES, so several draws share a base.
    // The whole-frame backend currently renders every draw into ONE host target
    // regardless of this base, which is why a deferred in-game frame -- many
    // surfaces -- comes out black while a one-surface menu does not. This tally
    // is the ground truth for the render-target cache (re-frontier gameplay-scene).
    // The mode breakdown is PER SURFACE, not just per frame: "353 draws target
    // the HDR world surface" and "how many of those can write colour at all"
    // are different questions, and only the second one explains an empty
    // surface. A frame-wide mode tally cannot answer it.
    struct SurfaceStat
    {
        uint32_t draws = 0; uint32_t format = 0; uint32_t mode = 0;
        uint32_t colorDepth = 0, depthOnly = 0, otherMode = 0;
    };
    std::map<uint32_t, SurfaceStat> surfaces; // RB_COLOR_INFO color_base -> stat
    // RB_MODECONTROL.edram_mode (0x2208, bits 0..2) per draw. This is NOT a
    // detail: on Xenos a draw with edram_mode == kCopy (6) is not geometry at
    // all, it is a RESOLVE -- the primitive selects the region of the EDRAM
    // surface to copy out to main memory (Xenia: VulkanCommandProcessor::
    // IssueDraw dispatches straight to IssueCopy on that mode). Rendering one
    // as if it were geometry paints the resolve rectangle's shader output into
    // the colour target. Counted here before anything acts on it.
    //   0 kNoOperation  4 kColorDepth  5 kDepthOnly  6 kCopy
    std::map<uint32_t, uint32_t> edramModes;
    // Draws issued with NO fragment stage because their edram_mode is not
    // kColorDepth. Counted, because "the frame got darker" and "36% of the
    // frame's draws stopped writing colour they should never have written" are
    // the same observation and only this number tells them apart.
    uint32_t drawsNoPixelShader = 0;
    // Every resolve of the frame, decoded per the Xenia contract: which colour
    // surface it reads (RB_COPY_CONTROL.copy_src_select indexes RB_COLOR_INFO
    // 0x2001/0x2003/0x2004/0x2005; >= 4 means depth) and where it writes
    // (RB_COPY_DEST_BASE 0x2319). This is the missing edge between "the draws
    // that wrote a surface" and "the later draws that sample it".
    struct ResolveEvent
    {
        uint32_t drawIndex = 0;   // position in the frame's draw list
        uint32_t srcSelect = 0;   // RB_COPY_CONTROL.copy_src_select
        bool srcIsDepth = false;
        uint32_t srcBase = 0;     // EDRAM tile base of the source surface
        uint32_t srcFormat = 0;   // its ColorRenderTargetFormat (colour only)
        uint32_t destBase = 0;    // RB_COPY_DEST_BASE, main memory
    };
    std::vector<ResolveEvent> resolves;
    std::set<uint32_t> depthBases;              // distinct RB_DEPTH_INFO.depth_base
    uint32_t issuedResolves = 0, skippedResolves = 0;
    std::map<uint64_t, uint64_t> skipReasons; // reason code -> count (for a summary)
    uint64_t texBindsStub = 0;   // texture bindings served by a stub image
    uint64_t texBindsRt = 0;     // texture bindings served by the rendered RT
    uint64_t texBindsGuest = 0;  // texture bindings served by real guest texture data
    // Geometry reach: how many draws fetch vertices from outside the SSBO
    // mirror. Such a fetch reads zero, so every primitive collapses -- and the
    // result looks exactly like "shaded black", which is why it is counted.
    uint64_t vfDrawsPastMirror = 0, vfDrawsInMirror = 0;
    uint32_t vfHighestByte = 0;
    std::map<std::string, uint64_t> viewportCensus; // guest viewport/scissor -> draws
    // Upload is on by default; GEARS_DRAW_NOTEX=1 is the control arm that
    // restores the stub-only frame for an A/B comparison.
    const bool texUploadEnabled = std::getenv("GEARS_DRAW_NOTEX") == nullptr;

    VkDescriptorImageInfo iiSamp{};
    iiSamp.sampler = samp;
    VkDescriptorBufferInfo biSsbo{ssbo, 0, VK_WHOLE_SIZE};

    // --- which guest address is "the render target this frame drew into" --
    // We do not model EDRAM. What we can read straight out of the frame's own
    // register snapshots is RB_COPY_DEST_BASE (0x2319): the main-memory address
    // the guest resolves the EDRAM surface to. A later draw that samples a
    // texture whose fetch-constant base address equals one of those resolve
    // destinations is sampling this frame's render target, and that is the link
    // between "the draws that wrote the scene" and "the passes that read it".
    // The scan is restricted to draws whose RB_MODECONTROL.edram_mode is kCopy,
    // because those are the frame's actual resolves. RB_COPY_DEST_BASE is a
    // sticky register: reading it on EVERY draw (which is what this did before)
    // reports every address any resolve ever set, long after that resolve, so
    // the destination set was both too large and unattributed to a source.
    // Pass one: which (base, format) surfaces the frame actually RENDERS into.
    // A resolve reprograms RB_COLOR_INFO to describe the source in whatever
    // format the copy reads it as -- measured on an Act 1 frame, EDRAM base
    // 0x2d0 is rendered as k_8_8_8_8 but resolved variously as
    // k_2_10_10_10_AS_10_10_10_10, k_2_10_10_10_FLOAT_AS_16_16_16_16, k_16_16
    // and k_2_10_10_10_FLOAT. That resolve-time format is a REINTERPRETATION of
    // the same EDRAM bytes, not a different surface, so a resolve's source is
    // looked up by BASE against the surfaces the frame renders, not by the
    // (base, format) pair its own registers happen to carry.
    for (const FrameDrawItem& d : in.draws)
    {
        if (d.registerFile.size() < 0x8000)
            continue;
        const uint32_t* R = d.registerFile.data();
        const uint32_t mode = R[0x2208] & 0x7;
        if (mode != 4 /*kColorDepth*/ && mode != 5 /*kDepthOnly*/)
            continue;
        formatsPerBase[R[0x2001] & 0xFFF].insert((R[0x2001] >> 16) & 0xF);
    }

    // Pass two: the frame's resolves, each given its destination's host image.
    std::set<uint32_t> resolveDests;
    uint32_t resolvesUnmatched = 0, resolvesDepth = 0;
    for (const FrameDrawItem& d : in.draws)
    {
        if (d.registerFile.size() < 0x8000)
            continue;
        const uint32_t* R = d.registerFile.data();
        if ((R[0x2208] & 0x7) != 6 /*kCopy*/)
            continue;
        const uint32_t srcSelect = R[0x2318] & 0x7;
        if (srcSelect >= 4) // depth resolve; no host depth texture chain yet
        { ++resolvesDepth; continue; }
        const uint32_t destBase = R[0x2319] & ~0xFFFu;
        if (!destBase)
            continue;
        static const uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
        const uint32_t info = R[kColorInfo[srcSelect & 3]];
        const uint32_t srcBase = info & 0xFFF;
        if (!formatsPerBase.count(srcBase))
        { ++resolvesUnmatched; continue; }
        ResolveTarget* rt = nullptr;
        if (!getResolveTarget(destBase, srcBase, rt))
            continue;
        resolveDests.insert(destBase);
    }
    if (resolvesDepth || resolvesUnmatched)
        lucent::info("draw", "frame resolves not served: {} from depth (no host depth"
            " texture chain yet), {} from an EDRAM base this frame never rendered",
            resolvesDepth, resolvesUnmatched);
    {
        lucent::Line rd;
        rd.add("frame: {} resolve destinations (from kCopy draws):",
               resolveDests.size());
        for (uint32_t b : resolveDests)
            rd.add(" {:#x}", b);
        rd.flush(lucent::Level::Info, "draw");
    }

    // Picks the image view for one texture binding. The stub matching the
    // shader's declared image dimension is the floor; a binding whose fetch
    // constant points at a resolve destination of THIS frame is served by the
    // rendered colour target instead (rtView, null until the segmented pass
    // below has something to give it).
    // GEARS_DRAW_NORT=1 is the control arm that restores the old behaviour of
    // decoding a resolve destination out of stale guest memory, for an A/B.
    const bool rtLinkEnabled = std::getenv("GEARS_DRAW_NORT") == nullptr;
    const bool listDraws = lucent::config::flag("DRAW_FRAME_LIST");
    std::map<uint32_t, uint64_t> texBaseCount;    // fetch base address -> bindings
    std::map<uint32_t, uint64_t> texBaseRtCount;  // ... restricted to resolve destinations
    auto selectTexView = [&](const uint32_t* R, const draw::ShaderTextureBinding& tb)
        -> VkImageView {
        const uint32_t fc = tb.fetchConstant & 31;
        const uint32_t dword1 = R[0x4800 + fc * 6 + 1];
        const uint32_t base = (dword1 >> 12) << 12;
        const bool isRt = base != 0 && resolveDests.count(base) != 0;
        ++texBaseCount[base];
        if (isRt)
            ++texBaseRtCount[base];
        // A binding that names a resolve destination of THIS frame reads that
        // destination's own host image -- the surface it was resolved from, in
        // that surface's format. Each destination has its own image, so two
        // passes sampling two different resolves no longer collide.
        if (isRt && rtLinkEnabled && tb.dimension <= 1)
        {
            auto rt = P.resolveTargets.find(base);
            if (rt != P.resolveTargets.end())
            {
                ++texBindsRt;
                return rt->second.view;
            }
        }
        // The guest's own texture, decoded from this fetch constant. The stub
        // below is only reached when the decode reports a reason it cannot.
        if (texUploadEnabled)
        {
            const auto tTex = Clock::now();
            VkImageView v = uploadTexture(&R[0x4800 + fc * 6], tb.dimension);
            accumulate(msTexture, tTex);
            if (v != VK_NULL_HANDLE)
            {
                ++texBindsGuest;
                return v;
            }
        }
        ++texBindsStub;
        switch (tb.dimension)
        {
            case 2: return stub3D.view;
            case 3: return stubCube.view;
            default: return stub2D.view;
        }
    };

    for (const FrameDrawItem& d : in.draws)
    {
        if (d.registerFile.size() < 0x8000)
        { ++skipped; ++skipReasons[1]; continue; }
        const uint32_t* R = d.registerFile.data();

        // Which EDRAM surface this draw targets (RB_COLOR_INFO: color_base is
        // the low 12 bits in tiles, color_format bits 16..19), and what the
        // draw is for (RB_MODECONTROL.edram_mode). Both are read before any
        // early exit so the census covers every draw, resolves included.
        const uint32_t surfaceBase = R[0x2001] & 0xFFF;
        const uint32_t edramMode = R[0x2208] & 0x7;
        {
            SurfaceStat& st = surfaces[surfaceBase];
            ++st.draws;
            st.format = (R[0x2001] >> 16) & 0xF;
            st.mode = edramMode;
            if (edramMode == 4) ++st.colorDepth;
            else if (edramMode == 5) ++st.depthOnly;
            else ++st.otherMode;
        }
        ++edramModes[edramMode];
        if (edramMode == 6 /*kCopy*/)
        {
            ResolveEvent re;
            re.drawIndex = uint32_t(&d - in.draws.data());
            re.srcSelect = R[0x2318] & 0x7;
            re.srcIsDepth = re.srcSelect >= 4; // kMaxColorRenderTargets
            static const uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
            const uint32_t info = re.srcIsDepth ? R[0x2002]
                                                : R[kColorInfo[re.srcSelect & 3]];
            re.srcBase = info & 0xFFF;
            re.srcFormat = (info >> 16) & 0xF;
            re.destBase = R[0x2319] & ~0xFFFu;
            resolves.push_back(re);
        }
        // How many distinct depth surfaces the frame uses. One host depth image
        // is shared by every colour target; this is the number that says when
        // that stops being faithful.
        depthBases.insert(R[0x2002] & 0xFFF);

        // A resolve is not geometry. It copies an EDRAM surface out to main
        // memory, and the primitive only selects the region; issuing it as a
        // draw paints the resolve rectangle's shader output over the surface.
        // Handled before anything else because it needs no shaders at all.
        if ((R[0x2208] & 0x7) == 6 /*kCopy*/)
        {
            const uint32_t srcSelect = R[0x2318] & 0x7;
            const uint32_t destBase = R[0x2319] & ~0xFFFu;
            static const uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
            const uint32_t srcBase = srcSelect < 4
                ? (R[kColorInfo[srcSelect & 3]] & 0xFFF) : 0xFFFFFFFFu;
            if (formatsPerBase.count(srcBase) && P.resolveTargets.count(destBase))
            {
                PreparedDraw pd{};
                pd.isResolve = true;
                pd.surfaceBase = srcBase;
                pd.resolveDest = destBase;
                prepared.push_back(pd);
                ++issuedResolves;
            }
            else
            {
                ++skippedResolves;
            }
            continue;
        }
        if (!d.vsUcode || !d.psUcode)
        { ++skipped; ++skipReasons[1]; continue; }

        draw::ShaderXlate *vsX = nullptr, *psX = nullptr;
        VkShaderModule vsMod = VK_NULL_HANDLE, psMod = VK_NULL_HANDLE;
        // The interpolator mask (and the rest of the modification) is a property
        // of this draw's VS+PS pair and its own registers, so it is derived
        // here, per draw, before either stage is translated.
        uint64_t vsModification = 0, psModification = 0;
        if (!draw::DeriveShaderModifications(R, d.vsUcode, d.vsUcodeSize, d.vsHash,
                d.psUcode, d.psUcodeSize, d.psHash, vsModification, psModification))
        { ++skipped; ++skipReasons[2]; continue; }
        if (!getShader(true, d.vsUcode, d.vsUcodeSize, d.vsHash, vsModification, vsX, vsMod) ||
            !getShader(false, d.psUcode, d.psUcodeSize, d.psHash, psModification, psX, psMod))
        { ++skipped; ++skipReasons[2]; continue; }

        VkDescriptorSetLayout vsTexLayout = 0, psTexLayout = 0;
        VkPipelineLayout pipeLayout = 0;
        if (!getPipeLayout(*vsX, *psX, vsTexLayout, psTexLayout, pipeLayout))
        { ++skipped; ++skipReasons[3]; continue; }
        // GEARS_DRAW_ONLY_BASE=<hex>: render only draws targeting one EDRAM
        // surface. A DIAGNOSTIC control arm -- it isolates one surface's
        // contribution -- never a fix.
        static const long onlyBase = lucent::config::number("DRAW_ONLY_BASE", -1);
        if (onlyBase >= 0 && surfaceBase != uint32_t(onlyBase))
        { ++skipped; ++skipReasons[0]; continue; }
        // This draw's own host render target, and the render pass its pipeline
        // must be built against.
        SurfaceTarget* target = nullptr;
        if (!getSurfaceTarget(surfaceBase, target))
        { ++skipped; ++skipReasons[8]; continue; }
        std::pair<VkRenderPass, VkRenderPass>* rp = nullptr;
        if (!getPasses(target->hostFormat, rp))
        { ++skipped; ++skipReasons[8]; continue; }
        OutputMergerState om;
        om.colorMask = R[0x2104];
        om.blend0 = R[0x2201];
        om.depthControl = R[0x2200];
        // Rectangle lists go through the geometry shader that builds the fourth
        // vertex. Everything else runs with no geometry stage at all.
        VkShaderModule gsMod = VK_NULL_HANDLE;
        if (d.primType == 8 /*kRectangleList*/)
        {
            ++rectDraws;
            if (getRectGeomShader(vsModification, gsMod))
                ++rectDrawsExpanded;
        }
        // The pixel shader runs ONLY when edram_mode is kColorDepth. This is
        // Xenia's contract (xenos.h EdramMode, vulkan_command_processor.cc
        // IssueDraw leaves pixel_shader null otherwise), and the evidence for it
        // is recorded in Xenia's own comment: titles bind shaders to kDepthOnly
        // draws that clearly belong to the colour pass -- shadowmap fetches and
        // all -- so the mode, not the binding, decides. A kDepthOnly draw that
        // runs its pixel shader writes colour the hardware never wrote.
        // GEARS_DRAW_DEPTHONLY_PS=1 restores the old behaviour: a DIAGNOSTIC
        // control arm for A/B-ing exactly this, never a fix.
        static const bool depthOnlyRunsPs =
            std::getenv("GEARS_DRAW_DEPTHONLY_PS") != nullptr;
        const bool pixelShaderUsed = edramMode == 4 /*kColorDepth*/ || depthOnlyRunsPs;
        if (!pixelShaderUsed)
            ++drawsNoPixelShader;
        VkPipeline pipe = VK_NULL_HANDLE;
        if (!getPipeline(vsMod, pixelShaderUsed ? psMod : VK_NULL_HANDLE, gsMod,
                         d.primType, om, rp->first, pipeLayout, pipe))
        { ++skipped; ++skipReasons[3]; continue; }

        // Per-draw constant UBOs from this draw's own register snapshot.
        std::vector<uint8_t> sysc = draw::DeriveSystemConstants(R);
        std::vector<uint8_t> fVs = PackFloatConstants(R, vsX->floatBitmap, vsX->floatCount, 0x4000);
        std::vector<uint8_t> fPs = PackFloatConstants(R, psX->floatBitmap, psX->floatCount, 0x4400);
        std::vector<uint8_t> boolLoop(sizeof(uint32_t) * (8 + 32));
        std::memcpy(boolLoop.data(), &R[0x4900], boolLoop.size());
        std::vector<uint8_t> fetch(sizeof(uint32_t) * 6 * 32);
        std::memcpy(fetch.data(), &R[0x4800], fetch.size());

        VkDescriptorBufferInfo biSys{}, biFvs{}, biFps{}, biBl{}, biFetch{};
        if (!makeUbo(sysc.data(), sysc.size(), biSys) ||
            !makeUbo(fVs.data(), fVs.size(), biFvs) ||
            !makeUbo(fPs.data(), fPs.size(), biFps) ||
            !makeUbo(boolLoop.data(), boolLoop.size(), biBl) ||
            !makeUbo(fetch.data(), fetch.size(), biFetch))
        { ++skipped; ++skipReasons[4]; continue; }

        // Index buffer: only for kDMA (indexed) draws. A kAutoIndex draw feeds
        // gl_VertexIndex = 0..count-1 directly (vkCmdDraw), matching how the
        // hardware sequences an auto-indexed primitive; the shader's vfetch then
        // reads the vertex from the SSBO by that index.
        //
        // kQuadList (0x0D) has no Vulkan topology. The hardware draws each
        // group of 4 vertices as a quad; Xenia's PrimitiveProcessor expands
        // that to a triangle list (0,1,2 / 0,2,3) rather than pretending a
        // quad list is a triangle list. Without the expansion the vertices are
        // regrouped into unrelated triangles, and this frame's ENTIRE world
        // geometry is quad_list -- it drew nothing.
        VkBuffer ibuf = VK_NULL_HANDLE;
        VkDeviceSize ibufOffset = 0;
        uint32_t drawCount = d.indexCount;
        bool drawIndexed = d.indexed;
        if (d.primType == 13 /*kQuadList*/)
        {
            const uint32_t quads = d.indexCount / 4;
            const uint32_t triIndices = quads * 6;
            if (quads == 0)
            { ++skipped; ++skipReasons[7]; continue; }
            // Guest indices (when present) are read first, then regrouped, so
            // the expansion works for both auto and DMA quad lists.
            std::vector<uint32_t> src(d.indexCount);
            if (d.indexed)
            {
                const uint8_t* base = in.guestBase + d.indexGuestBase;
                const uint32_t width = d.indexIs32 ? 4u : 2u;
                const bool inRange = d.indexGuestBase + d.indexCount * width <=
                                     in.guestPhysicalMirrorBytes;
                for (uint32_t i = 0; i < d.indexCount; ++i)
                {
                    uint32_t v = 0;
                    if (inRange && d.indexIs32)
                    { std::memcpy(&v, base + i * 4, 4); v = __builtin_bswap32(v); }
                    else if (inRange)
                    {
                        uint16_t h = 0; std::memcpy(&h, base + i * 2, 2);
                        v = uint16_t((h >> 8) | (h << 8));
                    }
                    src[i] = v;
                }
            }
            else
            {
                for (uint32_t i = 0; i < d.indexCount; ++i)
                    src[i] = i;
            }
            std::vector<uint32_t> expanded(triIndices);
            uint32_t* dst = expanded.data();
            for (uint32_t q = 0; q < quads; ++q)
            {
                const uint32_t* v = &src[q * 4];
                *dst++ = v[0]; *dst++ = v[1]; *dst++ = v[2];
                *dst++ = v[0]; *dst++ = v[2]; *dst++ = v[3];
            }
            if (!makeIndexBuffer(expanded.data(), triIndices * 4u, ibuf, ibufOffset))
            { ++skipped; ++skipReasons[5]; continue; }
            drawCount = triIndices;
            drawIndexed = true;
        }
        else if (d.indexed)
        {
            // The buffer is ALWAYS 32-bit: guest 16-bit indices are widened on
            // the way in. The draw binds VK_INDEX_TYPE_UINT32 unconditionally,
            // so sizing it by the guest's index width made every 16-bit indexed
            // draw read twice its buffer (validation
            // VUID-vkCmdDrawIndexed-robustBufferAccess2-08798) and rasterise
            // garbage indices.
            const uint32_t idxBytes = std::max(d.indexCount * 4u, 4u);
            std::vector<uint32_t> widened(idxBytes / 4, 0);
            void* p = widened.data();
            const uint8_t* base = in.guestBase + d.indexGuestBase;
            const bool inRange = d.indexGuestBase + idxBytes <= in.guestPhysicalMirrorBytes;
            if (d.indexIs32)
            {
                uint32_t* dst = static_cast<uint32_t*>(p);
                for (uint32_t i = 0; i < d.indexCount; ++i)
                {
                    uint32_t v = 0;
                    if (inRange) { std::memcpy(&v, base + i * 4, 4); v = __builtin_bswap32(v); }
                    dst[i] = v;
                }
            }
            else
            {
                uint32_t* dst = static_cast<uint32_t*>(p);
                const bool in16 = d.indexGuestBase + d.indexCount * 2u <=
                                  in.guestPhysicalMirrorBytes;
                for (uint32_t i = 0; i < d.indexCount; ++i)
                {
                    uint16_t v = 0;
                    if (in16) { std::memcpy(&v, base + i * 2, 2); v = uint16_t((v >> 8) | (v << 8)); }
                    dst[i] = v;
                }
            }
            if (!makeIndexBuffer(widened.data(), idxBytes, ibuf, ibufOffset))
            { ++skipped; ++skipReasons[5]; continue; }
        }

        // Descriptor sets for this draw. Sets 2/3 use this shader pair's own
        // texture layouts, so their binding counts match the SPIR-V exactly.
        VkDescriptorSet sets[4] = {};
        VkDescriptorSetLayout drawLayouts[4] = {set0, set1, vsTexLayout, psTexLayout};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 4;
        ai.pSetLayouts = drawLayouts;
        if (vkAllocateDescriptorSets(device, &ai, sets) != VK_SUCCESS)
        { ++skipped; ++skipReasons[6]; continue; }

        std::vector<VkWriteDescriptorSet> w;
        // Image infos must outlive the vkUpdateDescriptorSets call, so they are
        // held in a deque-stable store rather than a vector that may reallocate.
        std::deque<VkDescriptorImageInfo> imgInfos;
        auto setBuf = [&](VkDescriptorSet s, uint32_t b, VkDescriptorType t,
                          VkDescriptorBufferInfo* bi) {
            VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            ws.dstSet = s; ws.dstBinding = b; ws.descriptorCount = 1;
            ws.descriptorType = t; ws.pBufferInfo = bi;
            w.push_back(ws);
        };
        setBuf(sets[0], 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &biSsbo);
        setBuf(sets[1], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biSys);
        setBuf(sets[1], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biFvs);
        setBuf(sets[1], 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biFps);
        setBuf(sets[1], 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biBl);
        setBuf(sets[1], 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &biFetch);

        // One image per texture the shader declared, then one sampler each,
        // exactly in the translator's binding order.
        auto writeTextures = [&](const draw::ShaderXlate& x, VkDescriptorSet set) {
            for (uint32_t i = 0; i < uint32_t(x.textures.size()); ++i)
            {
                VkDescriptorImageInfo ii{};
                ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                ii.imageView = selectTexView(R, x.textures[i]);
                imgInfos.push_back(ii);
                VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                ws.dstSet = set; ws.dstBinding = i; ws.descriptorCount = 1;
                ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                ws.pImageInfo = &imgInfos.back();
                w.push_back(ws);
            }
            for (uint32_t j = 0; j < x.samplerCount; ++j)
            {
                // Sampler state is the guest's: filters and clamp modes come
                // from the fetch constant this sampler binding names.
                VkDescriptorImageInfo si = iiSamp;
                draw::GuestSamplerState gs;
                if (j < x.samplers.size() &&
                    draw::DeriveSamplerState(
                        &R[0x4800 + (x.samplers[j].fetchConstant & 31) * 6],
                        x.samplers[j], gs))
                    si.sampler = getSampler(gs);
                imgInfos.push_back(si);
                VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                ws.dstSet = set; ws.dstBinding = uint32_t(x.textures.size()) + j;
                ws.descriptorCount = 1;
                ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                ws.pImageInfo = &imgInfos.back();
                w.push_back(ws);
            }
        };
        writeTextures(*vsX, sets[2]);
        writeTextures(*psX, sets[3]);
        if (!w.empty())
            vkUpdateDescriptorSets(device, uint32_t(w.size()), w.data(), 0, nullptr);

        PreparedDraw pd{};
        pd.pipeline = pipe;
        pd.layout = pipeLayout;
        pd.sets[0] = sets[0]; pd.sets[1] = sets[1]; pd.sets[2] = sets[2]; pd.sets[3] = sets[3];
        pd.ibuf = ibuf;
        pd.ibufOffset = ibufOffset;
        pd.count = drawCount;
        pd.indexed = drawIndexed;
        pd.surfaceBase = surfaceBase;
        pd.diagIndex = uint32_t(&d - in.draws.data());
        pd.edramMode = edramMode;
        pd.primType = d.primType;
        pd.vsHash = d.vsHash;
        pd.psHash = d.psHash;
        pd.hasFragmentStage = pixelShaderUsed;
        pd.colorMask = om.colorMask;
        pd.depthControl = om.depthControl;
        pd.blend0 = om.blend0;
        pd.colorFormat = (R[0x2001] >> 16) & 0xF;
        pd.clipCntl = R[0x2204];
        pd.suScModeCntl = R[0x2205];
        pd.vteCntl = R[0x2206];
        pd.windowOffset = R[0x2080];
        std::memcpy(&pd.vportXScale, &R[0x210F], 4);
        std::memcpy(&pd.vportXOffset, &R[0x2110], 4);
        std::memcpy(&pd.vportYScale, &R[0x2111], 4);
        std::memcpy(&pd.vportYOffset, &R[0x2112], 4);
        std::memcpy(&pd.vportZScale, &R[0x2113], 4);
        std::memcpy(&pd.vportZOffset, &R[0x2114], 4);
        // Viewport/scissor from this draw's own registers, clamped to the host
        // target. A zero extent is a legitimately empty viewport on Xenos.
        {
            draw::GuestViewport gv;
            draw::DeriveViewport(R, gv);
            // GEARS_DRAW_FIXEDVP=1 restores the old host-fixed full-target
            // viewport: the control arm for measuring what the guest-derived
            // viewport/scissor changed.
            static const bool fixedVp = std::getenv("GEARS_DRAW_FIXEDVP") != nullptr;
            if (fixedVp)
            {
                gv.x = gv.y = gv.scissorX = gv.scissorY = 0;
                gv.w = gv.scissorW = W; gv.h = gv.scissorH = H;
                gv.zMin = 0.0f; gv.zMax = 1.0f;
            }
            ++viewportCensus[std::format("{},{} {}x{} scissor {},{} {}x{}",
                gv.x, gv.y, gv.w, gv.h, gv.scissorX, gv.scissorY,
                gv.scissorW, gv.scissorH)];
            pd.viewport.x = float(std::min(gv.x, W));
            pd.viewport.y = float(std::min(gv.y, H));
            pd.viewport.width = float(std::min(gv.w, W - std::min(gv.x, W)));
            pd.viewport.height = float(std::min(gv.h, H - std::min(gv.y, H)));
            pd.viewport.minDepth = gv.zMin;
            pd.viewport.maxDepth = gv.zMax;
            pd.scissor.offset = {int32_t(std::min(gv.scissorX, W)),
                                 int32_t(std::min(gv.scissorY, H))};
            pd.scissor.extent = {std::min(gv.scissorW, W - std::min(gv.scissorX, W)),
                                 std::min(gv.scissorH, H - std::min(gv.scissorY, H))};
        }
        // GEARS_DRAW_VDUMP=<draw index>: dump that draw's first vertices out of
        // the mirror, as the vertex shader's own fetch describes them (stride
        // from the shader's vertex binding, big-endian floats). Says whether a
        // draw that rasterises but shades black is fed real vertex data.
        {
            static const long vdump = lucent::config::number("DRAW_VDUMP", -1);
            if (vdump >= 0 && long(issued) == vdump)
            {
                for (const auto& vb : vsX->vertexBindings)
                {
                    const uint32_t fc = vb.fetchConstant & 95;
                    const uint32_t d0 = R[0x4800 + fc * 2];
                    if ((d0 & 3) != 3) continue;
                    const uint32_t vbase = (d0 >> 2) << 2;
                    const uint32_t stride = std::max(vb.strideWords, 1u);
                    for (uint32_t v = 0; v < 4; ++v)
                    {
                        lucent::Line vl;
                        vl.add("  draw {} vertex {} @ {:#x} (stride {} dwords):",
                               issued, v, vbase + v * stride * 4, stride);
                        for (uint32_t w = 0; w < stride; ++w)
                        {
                            const uint64_t off = uint64_t(vbase) + (v * stride + w) * 4;
                            if (off + 4 > in.guestPhysicalMirrorBytes) break;
                            uint32_t raw;
                            std::memcpy(&raw, in.guestBase + off, 4);
                            raw = __builtin_bswap32(raw);
                            float f;
                            std::memcpy(&f, &raw, 4);
                            vl.add(" [{}]{:#010x}={}", w, raw, f);
                        }
                        vl.flush(lucent::Level::Info, "draw");
                    }
                }
            }
        }
        // Geometry reach for this draw, counted whether or not the census is on.
        {
            bool anyPast = false, anyBinding = false;
            for (const auto& vb : vsX->vertexBindings)
            {
                const uint32_t fc = vb.fetchConstant & 95;
                const uint32_t d0 = R[0x4800 + fc * 2];
                const uint32_t d1 = R[0x4800 + fc * 2 + 1];
                if ((d0 & 3) != 3 /*kVertex*/)
                    continue;
                anyBinding = true;
                const uint64_t begin = uint64_t((d0 >> 2) << 2);
                const uint64_t end = begin + ((d1 >> 2) & 0xFFFFFF) * 4ull;
                vfHighestByte = std::max<uint32_t>(vfHighestByte, uint32_t(std::min<uint64_t>(end, 0xFFFFFFFFull)));
                if (end > in.guestPhysicalMirrorBytes)
                    anyPast = true;
                // This is the range the GPU will fetch from, so it is also
                // exactly the range that has to be uploaded.
                else if (end > begin)
                    fetchRanges.emplace_back(begin, end);
            }
            if (anyBinding)
                (anyPast ? vfDrawsPastMirror : vfDrawsInMirror) += 1;
            // A pixel shader may carry vertex fetches of its own. They are not
            // part of the geometry-reach census (which is about whether this
            // draw's GEOMETRY resolves), but they are still memory the GPU will
            // read, so they still have to be uploaded.
            for (const auto& vb : psX->vertexBindings)
            {
                const uint32_t fc = vb.fetchConstant & 95;
                const uint32_t d0 = R[0x4800 + fc * 2];
                const uint32_t d1 = R[0x4800 + fc * 2 + 1];
                if ((d0 & 3) != 3 /*kVertex*/)
                    continue;
                const uint64_t begin = uint64_t((d0 >> 2) << 2);
                const uint64_t end = begin + ((d1 >> 2) & 0xFFFFFF) * 4ull;
                if (end > begin && end <= in.guestPhysicalMirrorBytes)
                    fetchRanges.emplace_back(begin, end);
            }
        }
        if (listDraws)
        {
            lucent::Line dl;
            dl.add("  draw {}: {} {} {} verts, vs {:#018x} ({} tex) ps {:#018x} ({} tex),"
                   " colormask {:#x} blend {:#x} depth {:#x}",
                   issued, PrimName(d.primType), d.indexed ? "indexed" : "auto",
                   d.indexCount, d.vsHash, vsX->textures.size(), d.psHash,
                   psX->textures.size(), R[0x2104] /*RB_COLOR_MASK*/,
                   R[0x2201] /*RB_BLENDCONTROL0*/, R[0x2200] /*RB_DEPTHCONTROL*/);
            for (const auto& t : psX->textures)
                dl.add(" tex[fc{}]={:#x}", t.fetchConstant,
                       (R[0x4800 + (t.fetchConstant & 31) * 6 + 1] >> 12) << 12);
            // Where this draw's GEOMETRY comes from, and whether the SSBO
            // mirror actually covers it. A vfetch past the mirror reads zero,
            // which collapses every triangle -- indistinguishable in the output
            // from "shaded black", so it has to be reported explicitly.
            for (const auto& vb : vsX->vertexBindings)
            {
                const uint32_t fc = vb.fetchConstant & 95;
                const uint32_t d0 = R[0x4800 + fc * 2];
                const uint32_t d1 = R[0x4800 + fc * 2 + 1];
                const uint32_t vbase = (d0 >> 2) << 2;          // dword address -> bytes
                const uint32_t vbytes = ((d1 >> 2) & 0xFFFFFF) * 4; // size in dwords -> bytes
                dl.add(" vf[fc{}]type{}={:#x}+{:#x}{}", fc, d0 & 3, vbase, vbytes,
                       uint64_t(vbase) + vbytes <= in.guestPhysicalMirrorBytes
                           ? "" : " PAST-MIRROR");
            }
            // The float constants the shaders actually got. Nearly every pixel
            // shader in this frame ends in `mul oC0.xyz, r, c255.x`, so a zero
            // c255 makes the draw black no matter what it sampled.
            auto nonZero = [](const std::vector<uint8_t>& v) {
                size_t n = 0;
                for (size_t i = 0; i + 4 <= v.size(); i += 4)
                    if (v[i] || v[i + 1] || v[i + 2] || v[i + 3]) ++n;
                return n;
            };
            float psC255 = 0.0f;
            const uint32_t c255bits = R[0x4400 + 255 * 4];
            std::memcpy(&psC255, &c255bits, 4);
            dl.add(" vsconst {}/{} nz, psconst {}/{} nz, ps c255.x={} ({:#x})",
                   nonZero(fVs), vsX->floatCount, nonZero(fPs), psX->floatCount,
                   psC255, c255bits);
            dl.flush(lucent::Level::Info, "draw");
        }
        prepared.push_back(pd);
        ++issued;
    }

    // --- deferred range upload into the shared SSBO ----------------------
    // Only the memory this frame's draws fetch is copied. The mirror SPANS the
    // whole guest physical window so any fetch constant resolves, but a frame
    // touches a few MiB of it, and copying the span instead of the contents cost
    // more than the entire rest of the frame.
    //
    // Ranges are coalesced at page granularity first: a frame's vertex buffers
    // arrive as hundreds of small adjacent spans, and one memcpy per span is
    // dominated by per-call overhead rather than by the bytes.
    {
        const auto tUpload = Clock::now();
        constexpr uint64_t kPage = 0x1000;
        for (auto& r : fetchRanges)
        {
            r.first &= ~(kPage - 1);
            r.second = (r.second + kPage - 1) & ~(kPage - 1);
            r.second = std::min<uint64_t>(r.second, in.guestPhysicalMirrorBytes);
        }
        std::sort(fetchRanges.begin(), fetchRanges.end());
        uint64_t uploadedBytesSsbo = 0, spans = 0;
        for (size_t i = 0; i < fetchRanges.size();)
        {
            uint64_t begin = fetchRanges[i].first, end = fetchRanges[i].second;
            size_t j = i + 1;
            for (; j < fetchRanges.size() && fetchRanges[j].first <= end; ++j)
                end = std::max(end, fetchRanges[j].second);
            if (end > begin)
            {
                std::memcpy(static_cast<uint8_t*>(P.ssboMapped) + begin,
                            in.guestBase + begin, size_t(end - begin));
                uploadedBytesSsbo += end - begin;
                ++spans;
            }
            i = j;
        }
        accumulate(msSsboUpload, tUpload);
        if (in.report)
            lucent::info("draw", "frame guest-memory upload: {} KiB in {} spans"
                " (mirror spans {} MiB)", uploadedBytesSsbo / 1024, spans,
                in.guestPhysicalMirrorBytes >> 20);
    }

    // --- readback buffer -------------------------------------------------
    const VkDeviceSize rbBytes = VkDeviceSize(W) * H * 4;
    if (P.readback == VK_NULL_HANDLE)
    {
        if (!MakeBuffer(rbBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, P.readback,
                P.readbackMem, /*wantCached=*/true))
            return false;
        P.readbackBytes = rbBytes;
        VK_CHECK(vkMapMemory(device, P.readbackMem, 0, rbBytes, 0, &P.readbackMapped));
    }
    VkBuffer readback = P.readback;

    // --- command buffer: clear once, draw all in order -------------------
    VkCommandPool& cmdPool = P.cmdPool;
    if (cmdPool == VK_NULL_HANDLE)
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = queueFamily;
        VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &cmdPool));
    }
    VkCommandBuffer& cmd = P.cmd;
    if (cmd == VK_NULL_HANDLE)
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
    }
    VK_CHECK(vkResetCommandPool(device, cmdPool, 0));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &cbi));

    // Every stub image -> white -> shader-read, one per declared dimension.
    // Every resolve destination joins them but is cleared BLACK, not white:
    // until the frame's first resolve into it nothing has been rendered there,
    // and black says that honestly rather than washing the sampling pass white.
    std::vector<std::tuple<VkImage, uint32_t, bool>> initialClears{
        {stub2D.image, 1u, false}, {stub3D.image, 1u, false},
        {stubCube.image, 6u, false}};
    for (const auto& [destBase, rt] : P.resolveTargets)
        initialClears.emplace_back(rt.image, 1u, true);
    for (const auto& [img, layers, isRt] : initialClears)
    {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = 0; toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = img; toDst.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
        VkClearColorValue fill{};
        fill.float32[0] = fill.float32[1] = fill.float32[2] = isRt ? 0.0f : 1.0f;
        fill.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &fill, 1, &range);
        VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = img; toRead.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
    }

    // Guest textures: staging buffer -> image, once each, before any draw.
    for (const PendingUpload& u : uploads)
    {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, u.layers};
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = 0; toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = u.image; toDst.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
        // The decoded blob is tightly packed, layer-major, so one region with
        // zero row/image length (meaning "tightly packed") covers it.
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, u.layers};
        region.imageExtent = {u.w, u.h, u.d};
        vkCmdCopyBufferToImage(cmd, u.staging, u.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = u.image; toRead.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toRead);
    }

    VkClearValue clears[2]{};
    clears[0].color.float32[0] = 0.05f; // dark slate: any lit pixel is guest geometry
    clears[0].color.float32[1] = 0.05f;
    clears[0].color.float32[2] = 0.08f;
    clears[0].color.float32[3] = 1.0f;
    // Diagnostic only: the depth clear is still HOST-FIXED (the guest's own
    // comes from its clear packet / RB_DEPTH_CLEAR, which we do not yet track).
    // This frame's draws test GEQUAL, which is a reverse-Z convention, so 1.0
    // may be the wrong initial value -- GEARS_DRAW_DEPTH_CLEAR=<float> is the
    // control arm for measuring that, not a fix.
    {
        const char* dc = std::getenv("GEARS_DRAW_DEPTH_CLEAR");
        clears[1].depthStencil = {dc ? float(std::atof(dc)) : 1.0f, 0};
    }
    // Checkpoint dumps (GEARS_DRAW_FRAME_STEP=N): after every N draws the colour
    // target is copied to its own readback buffer and written out, so the frame
    // can be attributed to individual draws instead of guessed at.
    const long stepEvery = lucent::config::number("DRAW_FRAME_STEP", 0);
    std::vector<std::pair<uint32_t, VkBuffer>> checkpoints; // draws-so-far -> buffer
    std::vector<VkDeviceMemory> checkpointMem;

    // A surface's colour image leaves a render pass in TRANSFER_SRC_OPTIMAL, so
    // a resolve is: end the pass -> blit the source surface into the
    // destination's own host image -> begin the next pass with LOAD.
    //
    // The blit covers the WHOLE surface rather than the resolve rectangle. That
    // is consistent with how tiles are handled here: the console resolves once
    // per predicated tile, but our surface target already holds every tile
    // accumulated into one full-size image, so each tile's resolve carries the
    // whole (correct) surface and the last one leaves the destination right.
    // It follows the tile model rather than approximating around it.
    auto resolveSurfaceTo = [&](VkImage src, ResolveTarget& dst) {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dst.image; b.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        // Blit, not copy: source surfaces differ in format from the wide float
        // destination (see getResolveTarget), and only a blit converts.
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {int32_t(W), int32_t(H), 1};
        bl.dstOffsets[1] = {int32_t(W), int32_t(H), 1};
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
            VK_FILTER_NEAREST);
        VkImageMemoryBarrier r{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        r.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        r.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        r.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        r.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        r.srcQueueFamilyIndex = r.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        r.image = dst.image; r.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &r);
        ++dst.copies;
    };
    // Each checkpoint costs a full-frame readback buffer, so STEP=1 on a
    // 170-draw frame is capped rather than allocating 170 of them.
    const size_t kMaxCheckpoints = 48;
    // A checkpoint dumps the surface that was being rendered into at that
    // point, and only when that surface is 8888 -- the readback path is 8-bit
    // RGBA, and an HDR surface's bytes are not pixels.
    auto checkpointHere = [&](uint32_t drawsSoFar, const SurfaceTarget* t) {
        if (checkpoints.size() >= kMaxCheckpoints || !t ||
            t->hostFormat != VK_FORMAT_R8G8B8A8_UNORM || !t->begunThisFrame)
            return;
        VkBuffer b = 0; VkDeviceMemory m = 0;
        if (!MakeBuffer(rbBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b, m))
            return;
        VkBufferImageCopy rg{};
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, t->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            b, 1, &rg);
        checkpoints.emplace_back(drawsSoFar, b);
        checkpointMem.push_back(m);
    };

    // Per-draw pipeline statistics: how far each draw actually got through the
    // pipeline. Four counters per draw, in this order:
    //   0 input-assembly vertices, 1 input-assembly primitives,
    //   2 clipping primitives (what survived clip+cull), 3 fragment invocations.
    // A draw that adds no pixels is one of three very different things, and only
    // these numbers separate them: 0 primitives out of clipping (degenerate or
    // culled geometry), 0 fragment invocations (rasterised nothing), or many
    // fragment invocations (it ran and shaded/blended to nothing).
    // Not combinable with DRAW_ONLY: unwritten queries would never resolve.
    // GEARS_DRAW_DIAG=<path.tsv> writes the per-draw diagnostic table (below),
    // which is only useful joined with the pipeline statistics -- so it turns
    // them on rather than making the user remember two knobs.
    const char* diagPath = std::getenv("GEARS_DRAW_DIAG");
    const bool statsEnabled = (lucent::config::flag("DRAW_STATS") || diagPath) &&
                              hasPipelineStats &&
                              lucent::config::number("DRAW_ONLY", -1) < 0;
    const uint32_t kStatCounters = 4;
    VkQueryPool statPool = VK_NULL_HANDLE;
    if (statsEnabled)
    {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        qpi.queryCount = uint32_t(prepared.size());
        qpi.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        if (vkCreateQueryPool(device, &qpi, nullptr, &statPool) != VK_SUCCESS)
            statPool = VK_NULL_HANDLE;
        else
            vkCmdResetQueryPool(cmd, statPool, 0, uint32_t(prepared.size()));
    }

    // Every surface starts the frame un-begun, so the first draw into each one
    // CLEARS it and every draw after LOADS -- which is how the console's
    // predicated tiles accumulate into one host image per surface.
    for (auto& [k, s] : P.surfaceTargets)
    { s.begunThisFrame = false; s.drawsThisFrame = 0; }
    for (auto& [k, r] : P.resolveTargets)
        r.copies = 0;

    // GEARS_DRAW_ONLY=<index>: emit only that one draw, over the clear colour.
    // A DIAGNOSTIC control arm: it shows what a single draw's shader produces
    // without anything before it having painted the target, which is the only
    // way to tell "this draw contributes nothing" from "something later
    // overwrote it".
    const long onlyDraw = lucent::config::number("DRAW_ONLY", -1);
    uint32_t drawn = 0, segments = 0, surfaceSwitches = 0, resolvesDone = 0;
    // The surface a render pass is currently open on, if any.
    bool inPass = false;
    uint32_t openSurface = 0;
    SurfaceTarget* openTarget = nullptr;

    auto endPass = [&]() {
        if (!inPass)
            return;
        vkCmdEndRenderPass(cmd);
        inPass = false;
        openTarget = nullptr;
    };
    // Opens a pass on `key`'s target, clearing it if this frame has not touched
    // it yet and loading it otherwise.
    auto beginPassOn = [&](uint32_t base) -> bool {
        SurfaceTarget* t = nullptr;
        if (!getSurfaceTarget(base, t))
            return false;
        std::pair<VkRenderPass, VkRenderPass>* rp = nullptr;
        if (!getPasses(t->hostFormat, rp))
            return false;
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = t->begunThisFrame ? rp->second : rp->first;
        bi.framebuffer = t->fb;
        bi.renderArea = {{0, 0}, {W, H}};
        bi.clearValueCount = t->begunThisFrame ? 0u : 2u;
        bi.pClearValues = t->begunThisFrame ? nullptr : clears;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        t->begunThisFrame = true;
        inPass = true;
        openSurface = base;
        openTarget = t;
        ++segments;
        return true;
    };

    for (const PreparedDraw& pd : prepared)
    {
        if (pd.isResolve)
        {
            // The source surface must be outside a render pass to be blitted,
            // and it is left in TRANSFER_SRC_OPTIMAL by whichever pass wrote it.
            auto src = P.surfaceTargets.find(pd.surfaceBase);
            auto dst = P.resolveTargets.find(pd.resolveDest);
            if (src == P.surfaceTargets.end() || dst == P.resolveTargets.end() ||
                !src->second.begunThisFrame)
                continue; // nothing has been rendered into it yet this frame
            endPass();
            resolveSurfaceTo(src->second.color, dst->second);
            ++resolvesDone;
            continue;
        }
        if (onlyDraw >= 0 && long(drawn) != onlyDraw)
        { ++drawn; continue; }
        const bool needCheckpoint = stepEvery > 0 && drawn > 0 &&
                                    (drawn % uint32_t(stepEvery)) == 0;
        if (needCheckpoint)
        {
            endPass();
            checkpointHere(drawn, openTarget);
        }
        // Open a pass if there is none, or re-open on a different surface.
        if (!inPass || openSurface != pd.surfaceBase)
        {
            if (inPass)
            { endPass(); ++surfaceSwitches; }
            if (!beginPassOn(pd.surfaceBase))
                continue;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pd.pipeline);
        vkCmdSetViewport(cmd, 0, 1, &pd.viewport);
        vkCmdSetScissor(cmd, 0, 1, &pd.scissor);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pd.layout, 0,
            4, pd.sets, 0, nullptr);
        if (statPool != VK_NULL_HANDLE)
            vkCmdBeginQuery(cmd, statPool, drawn, 0);
        if (pd.indexed)
        {
            vkCmdBindIndexBuffer(cmd, pd.ibuf, pd.ibufOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, pd.count, 1, 0, 0, 0);
        }
        else
        {
            vkCmdDraw(cmd, pd.count, 1, 0, 0);
        }
        if (statPool != VK_NULL_HANDLE)
            vkCmdEndQuery(cmd, statPool, drawn);
        if (openTarget)
            ++openTarget->drawsThisFrame;
        ++drawn;
    }
    endPass();

    // --- which surface is presented ---------------------------------------
    // The frame's final composite is whatever surface the LAST geometry draw
    // wrote: on this title's UE3 pipeline that is the 8888 tonemap/UI buffer at
    // EDRAM 0x2d0, reached by rule rather than by naming the address.
    uint32_t presentBase = 0;
    bool havePresent = false;
    for (auto it = prepared.rbegin(); it != prepared.rend(); ++it)
    {
        if (it->isResolve)
            continue;
        presentBase = it->surfaceBase;
        havePresent = true;
        break;
    }
    SurfaceTarget* presentTarget = nullptr;
    if (havePresent)
    {
        auto it = P.surfaceTargets.find(presentBase);
        if (it != P.surfaceTargets.end() && it->second.begunThisFrame)
            presentTarget = &it->second;
    }
    if (presentTarget)
    {
        // Readback is 8-bit RGBA. A presented surface in any other host format
        // (an HDR one, if a frame ever ends on it) goes through a blit into an
        // 8888 staging image rather than being reinterpreted, which would read
        // the float bits as bytes.
        VkImage source = presentTarget->color;
        if (presentTarget->hostFormat != VK_FORMAT_R8G8B8A8_UNORM &&
            presentStage != VK_NULL_HANDLE)
        {
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = presentStage; b.subresourceRange = range;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            VkImageBlit bl{};
            bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            bl.srcOffsets[1] = {int32_t(W), int32_t(H), 1};
            bl.dstOffsets[1] = {int32_t(W), int32_t(H), 1};
            vkCmdBlitImage(cmd, presentTarget->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                presentStage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                VK_FILTER_NEAREST);
            VkImageMemoryBarrier r = b;
            r.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            r.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            r.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            r.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &r);
            source = presentStage;
        }
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readback, 1, &region);
    }
    VK_CHECK(vkEndCommandBuffer(cmd));

    P.arenaHighWater = std::max(P.arenaHighWater, arenaCursor);
    if (arenaOverflows)
        lucent::info("draw", "per-draw arena overflowed on {} allocations"
            " ({} KiB in use, {} KiB sized); next frame will fit",
            arenaOverflows, arenaCursor / 1024, P.arenaBytes / 1024);
    msDrawLoop = sinceStartMs() - msSetup;
    const auto tSubmit = Clock::now();
    VkFence& fence = P.fence;
    if (fence == VK_NULL_HANDLE)
    { VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VK_CHECK(vkCreateFence(device, &fi, nullptr, &fence)); }
    else
    { VK_CHECK(vkResetFences(device, 1, &fence)); }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    accumulate(msSubmit, tSubmit);

    if (statPool != VK_NULL_HANDLE)
    {
        std::vector<uint64_t> st(size_t(drawn) * kStatCounters, 0);
        if (drawn > 0 &&
            vkGetQueryPoolResults(device, statPool, 0, drawn,
                st.size() * sizeof(uint64_t), st.data(),
                kStatCounters * sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
        {
            uint32_t noPrims = 0, noFrags = 0, shaded = 0;
            for (uint32_t i = 0; i < drawn; ++i)
            {
                const uint64_t* s = &st[size_t(i) * kStatCounters];
                if (s[2] == 0) ++noPrims;
                else if (s[3] == 0) ++noFrags;
                else ++shaded;
                lucent::debug("draw", "  stats draw {}: {} verts, {} prims in,"
                    " {} prims after clip+cull, {} fragment invocations",
                    i, s[0], s[1], s[2], s[3]);
            }
            lucent::info("draw", "frame pipeline statistics: {} draws produced no"
                " primitive after clip+cull, {} produced primitives but no fragment,"
                " {} ran the fragment shader", noPrims, noFrags, shaded);

            // --- the per-draw diagnostic table -------------------------------
            // One row per issued draw, joining what the draw WAS (surface,
            // EDRAM mode, primitive, shaders) with what it DID (pipeline
            // statistics) and with every piece of state that can silently zero
            // it (depth test and function, colour mask, blend, viewport and
            // scissor extents, whether it even had a fragment stage).
            //
            // This table replaces guessing. "Surface 0x400 renders nothing" is
            // not actionable; "all 348 of surface 0x400's colour draws report
            // primitives in and zero primitives after clip+cull, and every one
            // of them has depth func GEQUAL against a cleared 1.0 depth buffer"
            // names the defect. Grouping and filtering belong to whatever reads
            // the TSV, so the renderer stays out of the analysis business.
            if (diagPath)
            {
                std::error_code ec;
                const std::filesystem::path dp(diagPath);
                if (dp.has_parent_path())
                    std::filesystem::create_directories(dp.parent_path(), ec);
                std::ofstream t(dp, std::ios::binary);
                if (!t)
                {
                    lucent::error("draw", "per-draw diagnostic: cannot write {}", diagPath);
                }
                else
                {
                    t << "draw\tsurface\tcolor_fmt\tedram_mode\tprim\tprim_name"
                         "\tindexed\tcount\tfrag_stage\tia_verts\tia_prims"
                         "\tprims_after_clip\tfrag_invocations\tverdict"
                         "\tdepth_test\tdepth_write\tdepth_func\tcolor_mask"
                         "\tblend_on\tblend0\tvp_x\tvp_y\tvp_w\tvp_h\tvp_minz"
                         "\tvp_maxz\tsc_x\tsc_y\tsc_w\tsc_h"
                         "\tclip_cntl\tsu_sc_mode\tvte_cntl\twindow_offset"
                         "\tvport_xs\tvport_xo\tvport_ys\tvport_yo"
                         "\tvport_zs\tvport_zo\tvs_hash\tps_hash\n";
                    uint32_t row = 0;
                    for (const PreparedDraw& pd : prepared)
                    {
                        if (pd.isResolve || row >= drawn)
                        { if (!pd.isResolve) ++row; continue; }
                        const uint64_t* s = &st[size_t(row) * kStatCounters];
                        // The verdict is the whole point: it says which stage
                        // this draw died at, in the vocabulary the pipeline
                        // statistics can actually support.
                        const char* verdict =
                            s[1] == 0 ? "no_primitive_assembled" :
                            s[2] == 0 ? "killed_by_clip_or_cull" :
                            s[3] == 0 ? "rasterised_no_fragment" :
                            !pd.hasFragmentStage ? "depth_only_no_colour" :
                            pd.colorMask == 0 ? "colour_fully_masked" : "shaded";
                        t << pd.diagIndex << '\t' << std::hex << "0x" << pd.surfaceBase
                          << std::dec << '\t' << pd.colorFormat << '\t' << pd.edramMode
                          << '\t' << pd.primType << '\t' << PrimName(pd.primType)
                          << '\t' << (pd.indexed ? 1 : 0) << '\t' << pd.count
                          << '\t' << (pd.hasFragmentStage ? 1 : 0)
                          << '\t' << s[0] << '\t' << s[1] << '\t' << s[2] << '\t' << s[3]
                          << '\t' << verdict
                          << '\t' << ((pd.depthControl >> 1) & 1)
                          << '\t' << ((pd.depthControl >> 2) & 1)
                          << '\t' << ((pd.depthControl >> 4) & 7)
                          << '\t' << (pd.colorMask & 0xF)
                          << '\t' << (BlendIsIdentity(pd.blend0) ? 0 : 1)
                          << '\t' << std::hex << "0x" << pd.blend0 << std::dec
                          << '\t' << pd.viewport.x << '\t' << pd.viewport.y
                          << '\t' << pd.viewport.width << '\t' << pd.viewport.height
                          << '\t' << pd.viewport.minDepth << '\t' << pd.viewport.maxDepth
                          << '\t' << pd.scissor.offset.x << '\t' << pd.scissor.offset.y
                          << '\t' << pd.scissor.extent.width << '\t' << pd.scissor.extent.height
                          << '\t' << std::hex << "0x" << pd.clipCntl
                          << '\t' << "0x" << pd.suScModeCntl
                          << '\t' << "0x" << pd.vteCntl
                          << '\t' << "0x" << pd.windowOffset << std::dec
                          << '\t' << pd.vportXScale << '\t' << pd.vportXOffset
                          << '\t' << pd.vportYScale << '\t' << pd.vportYOffset
                          << '\t' << pd.vportZScale << '\t' << pd.vportZOffset
                          << '\t' << std::hex << pd.vsHash << '\t' << pd.psHash
                          << std::dec << '\n';
                        ++row;
                    }
                    lucent::info("draw", "per-draw diagnostic: {} rows written to {}",
                                 row, diagPath);
                }
            }
        }
        vkDestroyQueryPool(device, statPool, nullptr);
    }

    // --- read pixels + coverage numbers ----------------------------------
    g_frame.resize(rbBytes);
    std::memcpy(g_frame.data(), P.readbackMapped, rbBytes);
    // The per-frame census -- a full per-pixel scan, a dozen summary lines and a
    // PPM write -- costs ~40 ms, which is most of a warm frame. It answers
    // "what did this frame do", so it belongs to a CAPTURE, not to every frame
    // of a live run; in.report selects the frames that get it.
    if (in.report)
    {
        // Two different numbers, because one alone lies. "Changed" counts pixels the
        // draws touched at all (!= the clear colour); "lit" counts pixels that carry
        // actual light (non-black). A frame painted uniformly black by a multiply
        // pass scores 100% changed and 0% lit -- reporting only the first read as
        // full coverage of a frame that shows nothing.
        uint64_t lit = 0, changed = 0;
        for (uint32_t i = 0; i < W * H; ++i)
        {
            const uint8_t* px = &g_frame[size_t(i) * 4];
            if (!(px[0] == 13 && px[1] == 13 && px[2] == 20)) // != the clear
                ++changed;
            if (px[0] || px[1] || px[2])
                ++lit;
        }
        std::set<std::pair<uint64_t, uint64_t>> pairs;
        for (const FrameDrawItem& d : in.draws)
            pairs.emplace(d.vsHash, d.psHash);
        lucent::info("draw", "frame: {} of {} draws issued, {} skipped; {} distinct shader"
            " pairs, {} distinct shaders, {} pipelines, {} texture layouts,"
            " {} pipeline layouts; {} texture bindings ({} guest textures,"
            " {} from the rendered RT, {} from a stub); {}/{} px non-black"
            " ({:.1f}%), {} px changed from the clear ({:.1f}%)",
            issued, in.draws.size(), skipped, pairs.size(), modules.size(), pipelines.size(),
            texLayouts.size(), pipeLayouts.size(),
            texBindsRt + texBindsStub + texBindsGuest, texBindsGuest, texBindsRt,
            texBindsStub, lit, uint64_t(W) * H, 100.0 * double(lit) / (double(W) * H),
            changed, 100.0 * double(changed) / (double(W) * H));
        if (rectDraws)
            lucent::info("draw", "frame rectangle lists: {} of {} draws expanded by a"
                " geometry shader ({} distinct)", rectDrawsExpanded, rectDraws,
                geomShaders.size());
        {
            lucent::Line sl;
            sl.add("frame EDRAM surfaces: {} distinct RB_COLOR_INFO bases"
                " (draws@base:fmt colour/depth-only/other):", surfaces.size());
            for (const auto& [base, st] : surfaces)
                sl.add(" {}@{:#x}:f{} {}/{}/{}", st.draws, base, st.format,
                       st.colorDepth, st.depthOnly, st.otherMode);
            sl.flush(lucent::Level::Info, "draw");
        }
        {
            lucent::Line ml;
            ml.add("frame EDRAM modes (RB_MODECONTROL.edram_mode):");
            for (const auto& [mode, n] : edramModes)
            {
                const char* name =
                    mode == 0 ? "no_op" : mode == 4 ? "color_depth" :
                    mode == 5 ? "depth_only" : mode == 6 ? "COPY(resolve)" : "?";
                ml.add(" {}={}", name, n);
            }
            ml.add("; {} draws issued with no fragment stage", drawsNoPixelShader);
            ml.flush(lucent::Level::Info, "draw");
        }
        {
            lucent::Line rl;
            rl.add("render-target cache: {} host surfaces (surface:format ->"
                   " host format, draws this frame):", P.surfaceTargets.size());
            for (const auto& [base, t] : P.surfaceTargets)
                rl.add(" {:#x}->vk{}x{}", base, uint32_t(t.hostFormat),
                       t.drawsThisFrame);
            rl.flush(lucent::Level::Info, "draw");
            lucent::Line dl;
            dl.add("render-target cache: {} resolve destinations (dest<-copies):",
                   P.resolveTargets.size());
            for (const auto& [base, r] : P.resolveTargets)
                dl.add(" {:#x}<-{}", base, r.copies);
            dl.flush(lucent::Level::Info, "draw");
            lucent::info("draw", "render-target cache: presented surface {:#x};"
                " resolves issued {}, skipped {}; {} distinct"
                " RB_DEPTH_INFO depth bases (one shared host depth image)",
                presentBase, issuedResolves, skippedResolves, depthBases.size());
        }
        {
            // One line per distinct (source surface -> destination) pair; a
            // frame resolves the same surface once per predicated tile, so the
            // raw event list is long and the pairing is what matters.
            std::map<std::string, uint32_t> pairs;
            for (const ResolveEvent& re : resolves)
                ++pairs[re.srcIsDepth
                    ? std::format("depth@{:#x} -> {:#x}", re.srcBase, re.destBase)
                    : std::format("color{}@{:#x}:f{} -> {:#x}", re.srcSelect,
                                  re.srcBase, re.srcFormat, re.destBase)];
            lucent::info("draw", "frame resolves: {} copy draws, {} distinct"
                " source->destination pairs", resolves.size(), pairs.size());
            for (const auto& [what, n] : pairs)
                lucent::info("draw", "  resolve {} x{}", what, n);
        }
        lucent::info("draw", "frame geometry reach: {} draws fetch vertices inside the"
            " {:#x}-byte SSBO mirror, {} draws fetch PAST it (those read zero and"
            " collapse); highest vertex-buffer end seen {:#x}",
            vfDrawsInMirror, in.guestPhysicalMirrorBytes, vfDrawsPastMirror, vfHighestByte);
        lucent::info("draw", "frame textures: {} distinct fetch constants, {} uploaded"
            " ({:.1f} MiB), {} samplers", texDistinct.size(), uploads.size(),
            double(uploadedBytes) / (1024.0 * 1024.0), samplerCache.size());
        for (const auto& [fmt, n] : texFormatBindings)
            lucent::info("draw", "  format {}: {} distinct fetches", fmt, n);
        for (const auto& [why, n] : texSkips)
            lucent::warn("draw", "  NOT uploaded, {} distinct fetches: {}", n, why);
        for (const auto& [what, n] : texFormatCensus)
            lucent::info("draw", "  texture {} x{}", what, n);
        for (const auto& [what, n] : viewportCensus)
            lucent::info("draw", "  guest viewport {} x{} draws", what, n);
        {
            std::map<uint32_t, uint32_t> prims;
            for (const FrameDrawItem& d : in.draws)
                ++prims[d.primType];
            lucent::Line pl;
            pl.add("frame primitive types:");
            for (const auto& [p, n] : prims)
                pl.add(" {}x{}", PrimName(p), n);
            pl.flush(lucent::Level::Info, "draw");
        }
        if (skipped)
        {
            for (const auto& [code, n] : skipReasons)
            {
                const char* why =
                    code == 1 ? "no snapshot/ucode" :
                    code == 2 ? "shader translate failed" :
                    code == 3 ? "pipeline create failed" :
                    code == 4 ? "UBO alloc failed" :
                    code == 5 ? "index buffer failed" :
                    code == 6 ? "descriptor alloc failed" :
                    code == 7 ? "quad list with fewer than 4 vertices" :
                    code == 8 ? "no host target for the draw's EDRAM surface format"
                              : "unknown";
                lucent::warn("draw", "  skipped {}x: {}", n, why);
            }
        }

        {
            lucent::Line tb;
            tb.add("frame texture bases ({} distinct):", texBaseCount.size());
            for (const auto& [base, n] : texBaseCount)
                tb.add(" {:#x}x{}{}", base, n, texBaseRtCount.count(base) ? "(RT)" : "");
            tb.flush(lucent::Level::Info, "draw");
        }
        lucent::info("draw", "frame render pass: {} segments across {} surface"
            " switches, {} resolves executed (RT link {})", segments,
            surfaceSwitches, resolvesDone, rtLinkEnabled ? "on" : "off");

        const char* dir = std::getenv("GEARS_DRAW_DIR");
        const std::filesystem::path reportDir =
            dir ? std::filesystem::path(dir) : std::filesystem::path("scratch/screenshots");
        std::filesystem::path out = in.sequence >= 0
            ? reportDir / std::format("frame_{:05}.ppm", in.sequence)
            : reportDir / "frame.ppm";
        if (WritePpm(out, g_frame.data(), W, H))
            lucent::info("draw", "frame screenshot written to {}", out.string());

    }
    msReadback = sinceStartMs() - msSetup - msDrawLoop - msSubmit;
    lucent::info("draw", "frame cost {:.0f} ms: setup {:.0f}, draw loop {:.0f}"
        " (of which shader translation {:.0f}, pipeline creation {:.0f},"
        " texture upload {:.0f}), guest-memory upload {:.0f}, submit+wait {:.0f},"
        " readback+report {:.0f}",
        sinceStartMs(), msSetup, msDrawLoop, msTranslate, msPipeline, msTexture,
        msSsboUpload, msSubmit, msReadback);
    // Checkpoint images, each labelled with how many draws had run.
    const char* checkpointDir = std::getenv("GEARS_DRAW_DIR");
    const std::filesystem::path outDir = checkpointDir
        ? std::filesystem::path(checkpointDir)
        : std::filesystem::path("scratch/screenshots");
    std::vector<uint8_t> cp(rbBytes);
    for (size_t i = 0; i < checkpoints.size(); ++i)
    {
        void* p = nullptr;
        if (vkMapMemory(device, checkpointMem[i], 0, rbBytes, 0, &p) != VK_SUCCESS)
            continue;
        std::memcpy(cp.data(), p, rbBytes);
        vkUnmapMemory(device, checkpointMem[i]);
        uint64_t cpLit = 0, cpNonBlack = 0;
        for (uint32_t k = 0; k < W * H; ++k)
        {
            const uint8_t* px = &cp[size_t(k) * 4];
            if (!(px[0] == 13 && px[1] == 13 && px[2] == 20))
                ++cpLit;
            if (px[0] || px[1] || px[2])
                ++cpNonBlack;
        }
        const std::string name = std::format("frame_after{:04}.ppm", checkpoints[i].first);
        WritePpm(outDir / name, cp.data(), W, H);
        lucent::info("draw", "  checkpoint after {} draws: {} px != clear, {} px non-black"
            " -> {}", checkpoints[i].first, cpLit, cpNonBlack, name);
    }

    // --- teardown --------------------------------------------------------
    for (size_t i = 0; i < checkpoints.size(); ++i)
    {
        vkDestroyBuffer(device, checkpoints[i].second, nullptr);
        vkFreeMemory(device, checkpointMem[i], nullptr);
    }
    // Only this frame's own transients are destroyed here. The render target,
    // passes, layouts, pipelines, shader modules, textures and samplers belong
    // to RendererPersistent and are released by ReleasePersistent.
    for (size_t i = 0; i < stagingBufs.size(); ++i)
    {
        vkDestroyBuffer(device, stagingBufs[i], nullptr);
        vkFreeMemory(device, stagingMems[i], nullptr);
    }
    for (size_t i = 0; i < keepBuffers.size(); ++i)
    {
        vkDestroyBuffer(device, keepBuffers[i], nullptr);
        vkFreeMemory(device, keepMem[i], nullptr);
    }
    P.built = true;
    return true;
}

} // namespace

bool RenderHotDraw(const HotDrawInputs& in)
{
    Renderer r;
    bool ok = false;
    if (r.Init())
        ok = r.Render(in);
    r.Shutdown();
    if (!ok)
        g_frame.clear();
    return ok;
}

// The renderer is built once and kept. Rebuilding it per frame was what made a
// frame cost ~300 ms; the device, render target, shader translations, pipelines
// and textures all survive from one frame to the next now.
Renderer& FrameRenderer()
{
    static Renderer r;
    static bool initialised = r.Init();
    (void)initialised;
    return r;
}

bool RenderFrame(const FrameDrawInputs& in)
{
    Renderer& r = FrameRenderer();
    if (r.device == VK_NULL_HANDLE)
    {
        g_frame.clear();
        return false;
    }
    const bool ok = r.RenderFrameImpl(in);
    if (!ok)
        g_frame.clear();
    return ok;
}

const std::vector<uint8_t>& GuestFramePixels() { return g_frame; }
uint32_t GuestFrameWidth() { return kWidth; }
uint32_t GuestFrameHeight() { return kHeight; }

} // namespace gears

#else // GEARS_HAVE_GUEST_DRAW

namespace gears
{
bool RenderHotDraw(const HotDrawInputs&)
{
    lucent::warn("draw", "built without the guest-draw backend"
        " (needs Vulkan + the Xenos translator)");
    return false;
}
bool RenderFrame(const FrameDrawInputs&)
{
    lucent::warn("draw", "built without the guest-draw backend"
        " (needs Vulkan + the Xenos translator)");
    return false;
}
const std::vector<uint8_t>& GuestFramePixels()
{
    static std::vector<uint8_t> empty;
    return empty;
}
uint32_t GuestFrameWidth() { return 0; }
uint32_t GuestFrameHeight() { return 0; }
} // namespace gears

#endif
