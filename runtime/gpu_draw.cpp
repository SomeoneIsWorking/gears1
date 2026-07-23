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

#include <lucent/log.h>

#ifdef GEARS_HAVE_GUEST_DRAW

#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
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

    bool Init();
    void Shutdown();
    bool FindMemory(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t& out);
    bool MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buf,
                    VkDeviceMemory& mem);
    bool Render(const HotDrawInputs& in);
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
                          VkBuffer& buf, VkDeviceMemory& mem)
{
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(device, &bi, nullptr, &buf));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buf, &req);
    uint32_t type = 0;
    if (!FindMemory(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            type))
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
    // Rectangle list has no direct Vulkan topology; the hot pair is a triangle
    // list, so anything else is reported and drawn as a triangle list.
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
    if (!draw::TranslateHotPair(in.vsUcode, in.vsUcodeSize, in.vsHash,
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
const std::vector<uint8_t>& GuestFramePixels()
{
    static std::vector<uint8_t> empty;
    return empty;
}
uint32_t GuestFrameWidth() { return 0; }
uint32_t GuestFrameHeight() { return 0; }
} // namespace gears

#endif
