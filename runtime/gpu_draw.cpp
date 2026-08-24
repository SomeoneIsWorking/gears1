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
#include "frame_ab.h"
#include "frame_artifact_policy.h"
#include "frame_probe_capture.h"
#include "gpu_device_features.h"
#include "guest_texture_hash.h"
#include "native_pass.h"
#include "spirv_clamp.h"
#include "gpu_shared_device.h"
#include "gpu_queue_family.h"
#include "gpu_queue_access.h"
#include <lucent/config.h>
#include <lucent/log.h>

#ifdef GEARS_HAVE_GUEST_DRAW

#include <algorithm>
#include <bit>
#include <cmath>
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
#include "gpu_draw_depth_bias.h"
#include "gpu_draw_formats.h"
#include "gpu_draw_pixels.h"
#include "gpu_draw_prepared.h"
#include "gpu_draw_untile.h"
#include "gpu_draw_renderer.h"
#include "gpu_draw_textures.h"
#include "gpu_draw_targets.h"
#include "gpu_draw_arena.h"
#include "gpu_draw_census.h"
#include "gpu_draw_descriptors.h"
#include "gpu_draw_pipelines.h"
#include "gpu_draw_probe.h"
#include "gpu_draw_resolve_decode.h"
#include "gpu_draw_resolve_plan.h"
#include "gpu_resolve_extent.h"
#include "gpu_draw_indices.h"
#include "gpu_draw_options.h"
#include "gpu_draw_uniforms.h"
#include "gpu_draw_vertexfetch.h"
#include "gpu_draw_shaders.h"

namespace gears
{
// Everything below is in gears::draw, the namespace the renderer's own headers
// use. It was an anonymous namespace until this file was split: a member of a
// class in an anonymous namespace can only be defined in the translation unit
// that declared it, which is precisely what made the 5300-line frame function
// impossible to take apart. Internal linkage is kept where it belongs -- on the
// file-local objects, with `static`.
namespace draw
{

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

// Accumulates the time a scope took into `sink`, HOWEVER the scope is left.
//
// The draw loop's per-draw body has several `continue` paths after the point
// where recording begins -- a failed descriptor allocation, an index buffer that
// would not fit. A plain "end minus begin" written at the bottom of the body
// drops every draw that took one of them, and those are exactly the draws an
// unexplained cost would be hiding in. The destructor cannot be skipped, so the
// measurement cannot quietly lose a case.
class ScopedMs
{
  public:
    explicit ScopedMs(double &sink) : sink_(sink), begin_(std::chrono::steady_clock::now()) {}
    ~ScopedMs()
    {
        sink_ +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin_)
                .count();
    }
    ScopedMs(const ScopedMs &) = delete;
    ScopedMs &operator=(const ScopedMs &) = delete;

  private:
    double &sink_;
    std::chrono::steady_clock::time_point begin_;
};

std::vector<uint8_t> g_frame; // last rendered R8G8B8A8 frame (file-local)

// VK_CHECK is in gpu_draw_renderer.h, shared with the renderer's other files.

bool Renderer::Init()
{
    // ADOPT THE PRESENTER'S DEVICE WHEN THERE IS ONE, rather than creating a second.
    // Two devices is what forces every rendered frame to be read back to host memory
    // and uploaded again through a staging buffer: the drawn image would live on one
    // device and the swapchain on the other, and nothing can be shared between them.
    //
    // The presenter is the side that publishes, because it comes up first in a
    // windowed run -- its thread starts at the first guest swap, this initialises at
    // GEARS_DRAW_FRAME_AT -- and because only it has a surface, so only it can pick a
    // queue family verified to present. It creates the device with this renderer's
    // features (gpu_device_features.h), which is what makes the device adoptable at
    // all.
    //
    // Headless there is no presenter and nothing to adopt, so the code below runs
    // exactly as before. The capability flags are derived from the physical device
    // rather than passed across, so both sides reach the same answers independently.
    {
        gears::SharedGpu shared;
        if (gears::AdoptSharedGpu(shared))
        {
            instance = shared.instance;
            physical = shared.physical;
            device = shared.device;
            queue = shared.queue;
            queueFamily = shared.queueFamily;
            ownsDevice = false;

            vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
            VkPhysicalDeviceProperties adoptedProps{};
            vkGetPhysicalDeviceProperties(physical, &adoptedProps);
            uniformOffsetAlignment =
                std::max<VkDeviceSize>(adoptedProps.limits.minUniformBufferOffsetAlignment, 4);
            maxViewportDim[0] = adoptedProps.limits.maxViewportDimensions[0];
            maxViewportDim[1] = adoptedProps.limits.maxViewportDimensions[1];
            hasStandardSampleLocations = adoptedProps.limits.standardSampleLocations == VK_TRUE;
            has2xFramebufferSamples =
                (adoptedProps.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0 &&
                (adoptedProps.limits.framebufferDepthSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0 &&
                (adoptedProps.limits.framebufferStencilSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0;

            VkPhysicalDeviceFeatures adoptedFeats{};
            gears::DeviceCapabilities adoptedCaps{};
            gears::SelectDeviceFeatures(physical, adoptedFeats, adoptedCaps);
            hasPipelineStats = adoptedCaps.pipelineStatistics;
            hasGeometryShader = adoptedCaps.geometryShader;
            hasStorageImageWithoutFormat = adoptedCaps.storageImageWithoutFormat;
            hasDepthClamp = adoptedCaps.depthClamp;
            hasSamplerAnisotropy = adoptedCaps.samplerAnisotropy;
            maxSamplerAnisotropy = adoptedProps.limits.maxSamplerAnisotropy;

            // Says only what is true. Sharing the device makes it POSSIBLE to
            // stop reading the frame back and re-uploading it, because the image
            // and the swapchain can now be the same object -- but that path is
            // still in place and still runs. Claiming the readback is gone here
            // would have a future reader believe a cost that is still being paid
            // has been removed.
            lucent::info("draw",
                         "adopted the presenter's Vulkan device \"{}\""
                         " (queue family {}); the rendered image and the swapchain are on ONE"
                         " device, so the frame reaches the window by BLIT rather than through"
                         " host memory",
                         adoptedProps.deviceName, queueFamily);
            return true;
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gears1-draw";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    const char *valLayer = "VK_LAYER_KHRONOS_validation";
    const char *dbgExt = "VK_EXT_debug_utils";
    if (lucent::config::flag("DRAW_VALIDATE"))
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
        // Shared with the present path (gpu_queue_family.h) so the two policies
        // cannot drift. needPresent is false here because this device draws into an
        // offscreen target and never touches a surface -- which is exactly the
        // split that costs a readback and a staging upload per frame today, and is
        // the next thing to remove.
        std::vector<gears::QueueFamily> caps(fc);
        for (uint32_t i = 0; i < fc; ++i)
        {
            caps[i].graphics = (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            caps[i].present = false;
            caps[i].count = fam[i].queueCount;
        }

        const uint32_t chosen = gears::ChooseQueueFamily(caps, /*needPresent=*/false);
        if (chosen == gears::kNoQueueFamily)
            continue;

        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(cand, &p);
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                             : 0;
        if (score > best)
        {
            best = score;
            physical = cand;
            queueFamily = chosen;
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
    maxViewportDim[0] = p.limits.maxViewportDimensions[0];
    maxViewportDim[1] = p.limits.maxViewportDimensions[1];
    hasStandardSampleLocations = p.limits.standardSampleLocations == VK_TRUE;
    has2xFramebufferSamples =
        (p.limits.framebufferColorSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0 &&
        (p.limits.framebufferDepthSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0 &&
        (p.limits.framebufferStencilSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0;

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    // The feature set lives in gpu_device_features.h, shared with the present path.
    // Whichever side ends up creating the single device must enable everything the
    // other will use, and the reasoning for each feature is recorded there.
    VkPhysicalDeviceFeatures feats{};
    gears::DeviceCapabilities caps{};
    gears::SelectDeviceFeatures(physical, feats, caps);
    hasPipelineStats = caps.pipelineStatistics;
    hasGeometryShader = caps.geometryShader;
    hasStorageImageWithoutFormat = caps.storageImageWithoutFormat;
    hasDepthClamp = caps.depthClamp;
    hasSamplerAnisotropy = caps.samplerAnisotropy;
    maxSamplerAnisotropy = p.limits.maxSamplerAnisotropy;

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

    // Publish ours in turn. Headless nothing adopts it, but a presenter starting
    // afterwards must find this device rather than create a second one.
    // checkedForPresent is false: this family was chosen with no surface to consult,
    // so its presentation support is unverified and the presenter must check.
    {
        gears::SharedGpu shared;
        shared.instance = instance;
        shared.physical = physical;
        shared.device = device;
        shared.queue = queue;
        shared.queueFamily = queueFamily;
        shared.checkedForPresent = false;
        gears::PublishSharedGpu(shared);
    }

    if (lucent::config::flag("DRAW_VALIDATE"))
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
            mi.pfnUserCallback =
                +[](VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
                    const VkDebugUtilsMessengerCallbackDataEXT *d, void *) -> VkBool32
            {
                lucent::warn("draw", "VK: {}", d->pMessage);
                return VK_FALSE;
            };
            create(instance, &mi, nullptr, &messenger);
        }
    }
    lucent::info("draw", "headless Vulkan device \"{}\" (queue family {})", p.deviceName,
                 queueFamily);
    return true;
}

bool Renderer::FindMemory(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t &out)
{
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & want) == want)
        {
            out = i;
            return true;
        }
    return false;
}

bool Renderer::MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buf,
                          VkDeviceMemory &mem, bool wantCached)
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

// ---------------------------------------------------------------------------
// Whole-frame rendering. Every draw of the frame is issued, in submission
// order, into ONE persistent colour+depth target inside a single render pass so
// the geometry accumulates. Each draw carries its own register-file snapshot
// (constants live at that draw) and its own bound shader pair; distinct shader
// pairs are translated and their pipelines/modules cached across the frame.
bool Renderer::RenderFrameImpl(const FrameDrawInputs &in)
{
    const uint32_t W = in.width ? in.width : kWidth;
    const uint32_t H = in.height ? in.height : kHeight;
    const draw::FrameOptions options = draw::ReadFrameOptions();

    // Phase timing names the owner of the cost instead of assuming it is the
    // caches.
    using Clock = std::chrono::steady_clock;
    const auto tStart = Clock::now();
    auto sinceStartMs = [&]
    { return std::chrono::duration<double, std::milli>(Clock::now() - tStart).count(); };
    double msSetup = 0, msDrawLoop = 0, msSubmit = 0, msReadback = 0;
    // Inside the draw loop. Recording a gameplay frame costs ~31 ms of CPU
    // for ~700 draws and the existing breakdown accounts for only ~2 of it,
    // so the rest is measured rather than guessed at -- hoisting the loop's
    // containers out was tried first on a hunch and moved it by 1 ms.
    double msUniforms = 0, msIndex = 0;
    // Uniform-cache accounting; see the hit test in the draw loop.
    double msState = 0, msRecord = 0, msCensus = 0;
    // Inside the record region, which is the biggest item in a gameplay frame.
    // DB.msWrite is a SUPERSET of DB.msUpdate: it is assembling the writes AND
    // submitting them, and the submit measures at ~0, so the two together say
    // whether the cost is ours or the driver's.
    double msPrepare = 0;
    // Inside state+pipeline, whose own 13-14 ms is the largest item in a
    // gameplay frame and the only one still unexplained inside itself.
    double msModify = 0, msShaderLookup = 0;

    // GEARS_DRAW_AB_CENSUS=1 puts the per-draw viewport census back on alternate
    // frames, so the arm with it and the arm without it are compared inside ONE
    // run. Separate runs cannot answer this: three of them at an identical 743
    // draws a frame gave 39.4, 47.2 and 42.7 ms draw loops, and the two slower
    // ones were the runs with the census already removed. See runtime/frame_ab.h.
    static AbTest ab(lucent::config::flag("DRAW_AB_CENSUS"));
    ab.BeginFrame();
    // GEARS_DRAW_AB_UNTILE=1 does the same for the EDRAM-tiling collapse: the
    // arm with tiling collapsed and the arm without it alternate frame by frame
    // inside one run. The collapse removes a QUARTER of a gameplay frame's draws,
    // and the honest thing to say about a saving that large is still a measured
    // number rather than two replay timings -- one of which came out slower.
    static AbTest abUntile(lucent::config::flag("DRAW_AB_UNTILE"));
    abUntile.BeginFrame();
    // And for the soft-dirty texture-staleness skips (catalog #137): the
    // baseline arm re-hashes every cached texture, the experimental arm skips
    // the ones whose guest pages the kernel reports unwritten.
    static AbTest abTexDirty(lucent::config::flag("DRAW_AB_TEXDIRTY"));
    abTexDirty.BeginFrame();
    {
        const int armed = int(ab.Enabled()) + int(abUntile.Enabled()) + int(abTexDirty.Enabled());
        if (armed > 1)
            lucent::error("draw", "more than one GEARS_DRAW_AB_* timing knob is on."
                                  " They alternate independently and all record the same frame"
                                  " cost, so no result would mean anything. Enable ONE");
    }
    // ON BY DEFAULT. A renderer that is native does not emulate the console's
    // EDRAM tiling, and this one no longer does: GEARS_DRAW_TILED=1 puts the
    // faithful per-tile replay back for an A/B or a bisect.
    //
    // The default was flipped on evidence, and the evidence is not unanimous.
    // Collapsing is bit-exact against the tiled path on act1_v2, bright and
    // play_v2, and differs on 197 of 2,764,800 channel samples on courtyard --
    // all inside the replayed band, caused by seam-crossing triangles being
    // rasterised once instead of twice-and-translated (claim C007). It CANNOT be
    // bit-exact, because not translating the world is the whole point. It costs
    // no measurable time either way (C008). So this is a change of posture, not
    // an optimisation: the renderer now does the host thing by default and the
    // console thing on request.
    const bool untileThisFrame =
        abUntile.Enabled() ? abUntile.Arm() : !lucent::config::flag("DRAW_TILED");
    double msSsboUpload = 0;
    auto accumulate = [](double &into, Clock::time_point from)
    { into += std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };

    // --- the EDRAM SAMPLE GRID ---------------------------------------------
    //
    // EDRAM is addressed in SAMPLES. A draw's viewport and scissor are in
    // PIXELS of its own surface, and RB_SURFACE_INFO.msaa_samples says how many
    // samples each of those pixels covers -- so a 4X 640x360 surface and a 1X
    // 1280x720 one are the same tiles, sample for sample. Rendering both at
    // their pixel sizes into one image puts the 4X draw in a corner of the 1X
    // one, which is what makes this title's shadow-mask fill cover a quadrant
    // where the console's covers the frame (catalog #91).
    //
    // So the host image is the SAMPLE grid, and every draw is scaled into it by
    // its own sample scale. The grid is as wide as the widest sample pitch the
    // frame programs and as tall as the frame's tallest sample scale needs:
    // measured on walk_gameplay.gfr, 1280 x 1440 (a 2X 1280x720 scene, a 1X
    // 1280x720 post chain and a 4X 640x360 fill, all at sample pitch 1280).
    //
    // ON BY DEFAULT, because it is measurably closer to the console. Paired
    // captures of the same SP_Prison_P moment, mean |difference| per pass:
    //
    //     pass                pixels   samples   console mean
    //     srcC2D0 f6 #0        0.551     0.217         0.8981
    //     srcC2D0 f6 #1        0.479     0.076         0.9238
    //     srcC2D0 f25 #0       0.250     MATCH         0.0000
    //
    // The last one was PREDICTED before the run (catalog #94): that copy reads
    // a surface written only by the aliasing pass, whose fill covers a quadrant
    // at pixel scale and the whole surface in samples, so at pixel scale three
    // quarters of it keep the scene depth and alias into colour as garbage.
    // The console resolves it at exactly 0.0000, and so do we now.
    //
    // GEARS_DRAW_NOMSAA=1 is the control arm: every draw at its pixel size into
    // a W x H image, which is what this renderer did for its whole life and is
    // self-consistent for any frame that does not change sample count on one
    // EDRAM base. This one changes it three times on surface 0x2d0.
    static const bool msaaModel = !lucent::config::flag("DRAW_NOMSAA");
    uint32_t SW = W, SH = H;
    {
        uint32_t maxScaleY = 1, maxSamplePitch = W;
        std::set<uint32_t> seen;
        for (const FrameDrawItem &d : in.draws)
        {
            const uint32_t *R = d.registers();
            if (!R)
                continue;
            const uint32_t si = R[0x2000];
            const uint32_t msaa = (si >> 16) & 3;
            seen.insert(msaa);
            maxScaleY = std::max(maxScaleY, draw::MsaaScaleY(msaa));
            maxSamplePitch = std::max(maxSamplePitch, (si & 0x3FFF) * draw::MsaaScaleX(msaa));
        }
        if (msaaModel)
        {
            SW = std::max(W, maxSamplePitch);
            SH = H * maxScaleY;
        }
        // ALWAYS SAID, in both arms. A frame with one sample count everywhere
        // needs none of this and a frame with several is wrong without it, and
        // silence cannot tell those apart.
        if (in.report)
        {
            lucent::Line l;
            l.add("EDRAM sample grid: {}x{} ({}), sample counts programmed:", SW, SH,
                  msaaModel ? "sample model on" : "OFF -- GEARS_DRAW_NOMSAA is set");
            for (uint32_t m : seen)
                l.add(" {}X", 1u << m);
            if (seen.size() > 1 && !msaaModel)
                l.add("; MORE THAN ONE, so draws on a shared EDRAM base are"
                      " being rendered at the same scale into one image when"
                      " the console gives them different ones");
            l.flush(lucent::Level::Info, "draw");
        }
    }

    // The persistent sample-grid allocation grows but never shrinks between
    // frames. gpu_renderer_capacity.cpp owns its cross-thread lifetime rule.
    EnsurePersistentCapacity(SW, SH);
    RendererPersistent &P = *persistent;
    const bool firstFrame = !P.built;

    // Shader translation and its cache live in gpu_draw_shaders.{h,cpp}: the
    // key is (microcode hash, modification, clamp), and a native pass is
    // substituted there, at the module, so the rest of the frame keeps the
    // guest shader's binding layout and constant map.
    draw::ShaderCache SC(*this, P);

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
        P.ssbo = VK_NULL_HANDLE;
        P.ssboMem = VK_NULL_HANDLE;
    }
    if (P.ssbo == VK_NULL_HANDLE)
    {
        if (!MakeBuffer(in.guestPhysicalMirrorBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, P.ssbo,
                        P.ssboMem))
            return false;
        P.ssboBytes = in.guestPhysicalMirrorBytes;
    }
    VkBuffer ssbo = P.ssbo;
    if (!P.ssboMapped)
        VK_CHECK(vkMapMemory(device, P.ssboMem, 0, in.guestPhysicalMirrorBytes, 0, &P.ssboMapped));
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
    StubTex &stub2D = P.stub2D;
    StubTex &stub3D = P.stub3D;
    StubTex &stubCube = P.stubCube;
    VkSampler &samp = P.stubSampler;
    auto makeStub = [&](VkImageType imageType, VkImageViewType viewType, uint32_t layers,
                        uint32_t depth3d, VkImageCreateFlags flags, StubTex &out) -> bool
    {
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
    if (firstFrame && (!makeStub(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 1, 0, stub2D) ||
                       !makeStub(VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 1, 0, stub3D) ||
                       !makeStub(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, 1,
                                 VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, stubCube)))
        return false;
    if (firstFrame)
    {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(device, &si, nullptr, &samp));
    }

    // Guest textures and samplers live in gpu_draw_textures.{h,cpp}. Its
    // counters are read by the frame report below, and every refusal it
    // records is printed there -- a skipped texture that is not reported is a
    // texture silently replaced by a stub.
    draw::TextureUploader TX(*this, P, in);
    // One soft-dirty observation period per rendered frame. The A/B arm
    // decides whether THIS frame may skip hashing page-clean textures; the
    // clear itself runs for both arms so generations stay aligned.
    TX.BeginStalenessFrame(!abTexDirty.Enabled() || abTexDirty.Arm());
    std::vector<VkBuffer> &stagingBufs = TX.stagingBufs;
    std::vector<VkDeviceMemory> &stagingMems = TX.stagingMems;

    // --- depth, one target per RB_DEPTH_INFO.depth_base ---------------------
    // D32_SFLOAT_S8_UINT, not D32_SFLOAT: the guest's depth buffer carries an
    // 8-bit STENCIL alongside the depth, and this title's shadow passes mark
    // stencil and then confine later passes to the marked pixels. Without a
    // stencil aspect the stencil state a pipeline declares is ignored and every
    // one of those passes runs over the whole screen (catalog #91). The depth
    // half stays 32-bit float, which holds both guest depth formats (kD24S8's
    // unorm24 and kD24FS8's float24) without banding.
    const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
    // The aspects the depth image is addressed with, in one place: every
    // barrier, view and clear below must agree with the format above, and a
    // mismatch is a validation error rather than a wrong picture.
    constexpr VkImageAspectFlags kDepthAspects =
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    // The IMAGES are created on demand by RenderTargetCache::GetDepthTarget,
    // ONE PER DEPTH BASE, because the guest uses more than one and sharing a
    // single image let the shadow atlas overwrite the scene's stencil
    // (catalog #91). Which bases a frame uses is not known until its draws are
    // prepared, so nothing is allocated here.
    if (firstFrame && !P.scanout.Initialize(*this, W, H))
        return false;

    // The render-target cache -- one host colour target per EDRAM surface, one
    // host image per resolve destination, the render passes each host format
    // needs, and the resolve compute pipeline -- is in gpu_draw_targets.{h,cpp}.
    // The cache's extents are the SAMPLE grid: its images are EDRAM, and
    // EDRAM is samples. Off the model SW,SH are W,H.
    draw::RenderTargetCache RT(*this, P, in, P.width, P.height, depthFormat);
    RT.BuildResolvePipeline();
    // GEARS_DRAW_REINTERP=1: convert a surface's contents when the frame
    // re-declares its EDRAM base under a different colour format. Off by
    // default until it is verified against a frame -- see
    // gpu_draw_reinterpret.cpp for what it is and what it fixes.
    // EDRAM format reinterpretation, ON by default since the resolve read its
    // source format from the right register (RB_COLOR_INFO[copy_src_select],
    // not RT0). It shipped off while it blew the picture out; that was this
    // bug, not the mechanism. GEARS_DRAW_NOREINTERP=1 is the control arm.
    const bool reinterpretEnabled = !lucent::config::flag("DRAW_NOREINTERP");
    if (reinterpretEnabled)
        RT.BuildReinterpretPipeline();
    // The EDRAM colour/depth aliasing pass shares the reinterpretation's knob:
    // both exist because EDRAM is one memory addressed by a base and this
    // renderer holds images. GEARS_DRAW_NOREINTERP=1 turns off both.
    if (reinterpretEnabled)
        RT.BuildDepthAliasPipeline();
    // GEARS_DRAW_REINTERP_SELFTEST=1 proves the conversion on this GPU rather
    // than on paper, once, before the frame it would otherwise silently alter.
    if (lucent::config::flag("DRAW_REINTERP_SELFTEST") && !P.reinterpretSelfTested)
    {
        P.reinterpretSelfTested = true;
        RT.ReinterpretSelfTest();
    }

    // Descriptor set layouts, pipeline layouts, rectangle geometry shaders and
    // the graphics pipelines are in gpu_draw_pipelines.{h,cpp}.
    draw::PipelineCache PC(*this, P);
    if (firstFrame && !PC.Build())
        return false;

    // --- descriptor pool sized for every draw ----------------------------
    msSetup = sinceStartMs();
    const uint32_t nDraws = uint32_t(in.draws.size());
    VkDescriptorPool &pool = P.descriptorPool;
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

    // The per-draw arena -- uniform blocks and expanded index buffers
    // suballocated from one persistently-mapped buffer -- lives in
    // gpu_draw_arena.{h,cpp}, with the fallback a frame takes when it outgrows
    // it and the high-water mark that sizes the next one.
    draw::FrameArena AR(*this, P);
    if (!AR.Build(nDraws))
        return false;

    using draw::PreparedDraw;
    std::vector<PreparedDraw> prepared;
    uint32_t issued = 0;
    // What the frame CONTAINED -- the per-surface, per-mode, reach, viewport and
    // skip tallies -- is in gpu_draw_census.{h,cpp}, with the report lines that
    // consume it.
    draw::FrameCensus CN;
    // Every resolve of the frame, decoded per the Xenia contract: which colour
    // surface it reads (RB_COPY_CONTROL.copy_src_select indexes RB_COLOR_INFO
    // 0x2001/0x2003/0x2004/0x2005; >= 4 means depth) and where it writes
    // (RB_COPY_DEST_BASE 0x2319). This is the missing edge between "the draws
    // that wrote a surface" and "the later draws that sample it".
    struct ResolveEvent
    {
        uint32_t drawIndex = 0; // position in the frame's draw list
        uint32_t srcSelect = 0; // RB_COPY_CONTROL.copy_src_select
        bool srcIsDepth = false;
        uint32_t srcBase = 0;   // EDRAM tile base of the source surface
        uint32_t srcFormat = 0; // its ColorRenderTargetFormat (colour only)
        uint32_t destBase = 0;  // RB_COPY_DEST_BASE, main memory
        // A resolve can also CLEAR the surface it copies from: RB_COPY_CONTROL
        // carries colour/depth clear enables, and the values live in
        // RB_COLOR_CLEAR / RB_DEPTH_CLEAR. This is where the guest's own clear
        // values come from -- our depth clear is still a host constant, which
        // rejects every reverse-Z draw (catalog #31).
        bool colorClear = false, depthClear = false;
        uint32_t copyCommand = 0;
        uint32_t colorClearValue = 0, depthClearValue = 0;
        uint32_t depthBase = 0, depthFormat = 0;
        // The resolve RECTANGLE, as the guest wrote it. Xenia's GetResolveInfo:
        // "D3D9 HACK: Vertices to use are always in vf0, and are written by the
        // CPU" -- 3 vertices of 2 floats, big-endian, from vertex fetch
        // constant 0. Reported raw, with the window offset alongside rather
        // than folded in, so the measurement does not depend on getting the
        // top-left fixed-point rounding right.
        bool haveRect = false;
        float rect[6] = {0, 0, 0, 0, 0, 0};
        int32_t windowX = 0, windowY = 0;
        // Where the copy LANDS: RB_COPY_DEST_PITCH (pitch and height, 14 bits
        // each) and RB_COPY_DEST_INFO's copy_dest_format. Without these the
        // destination is an address with no shape, and two tiles of one texture
        // cannot be told from two unrelated textures.
        uint32_t destPitch = 0, destHeight = 0, destFormat = 0;
        // RB_COPY_DEST_INFO's copy_dest_exp_bias: a SIGNED 6-bit exponent bias
        // the resolve applies to the colour on its way out. We ignore it, and
        // ignoring a negative bias makes everything 2^|bias| too bright -- which
        // is the shape of the frame's overexposure, so it is measured first.
        int32_t destExpBias = 0;
        uint32_t destEndian = 0, destSwap = 0, destNumber = 0;
    };
    std::vector<ResolveEvent> resolves;
    // Upload is on by default; GEARS_DRAW_NOTEX=1 is the control arm that
    // restores the stub-only frame for an A/B comparison.
    const bool texUploadEnabled = !lucent::config::flag("DRAW_NOTEX");

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
    // Which surfaces the frame renders, and where each resolve lands, are
    // worked out in gpu_draw_resolve_plan.{h,cpp} before any draw is prepared.
    const draw::ResolvePlan plan = draw::PlanResolves(in, RT);
    // One descriptor set per resolve this frame. They are tiny (two storage
    // images) and the pool is rebuilt only when a frame needs more than the
    // last one did.
    if (P.resolvePipeline != VK_NULL_HANDLE)
    {
        const uint32_t want = std::max<uint32_t>(8, plan.drawCount + 4);
        if (P.resolveDescPool == VK_NULL_HANDLE || P.resolveDescCapacity < want)
        {
            vkDestroyDescriptorPool(device, P.resolveDescPool, nullptr);
            P.resolveDescPool = VK_NULL_HANDLE;
            const VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                                 want * 3}, // colour sets use 2 each, depth sets 1
                                                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, want}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = want * 2; // colour sets and depth sets
            pi.poolSizeCount = 2;
            pi.pPoolSizes = ps;
            if (vkCreateDescriptorPool(device, &pi, nullptr, &P.resolveDescPool) == VK_SUCCESS)
                P.resolveDescCapacity = want;
        }
        if (P.resolveDescPool != VK_NULL_HANDLE)
        {
            vkResetDescriptorPool(device, P.resolveDescPool, 0);
            std::vector<VkDescriptorSetLayout> layouts(P.resolveDescCapacity, P.resolveSetLayout);
            RT.resolveSets.resize(P.resolveDescCapacity);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = P.resolveDescPool;
            ai.descriptorSetCount = P.resolveDescCapacity;
            ai.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(device, &ai, RT.resolveSets.data()) != VK_SUCCESS)
                RT.resolveSets.clear();
            if (P.resolveDepthSetLayout != VK_NULL_HANDLE)
            {
                std::vector<VkDescriptorSetLayout> dlayouts(P.resolveDescCapacity,
                                                            P.resolveDepthSetLayout);
                RT.resolveDepthSets.resize(P.resolveDescCapacity);
                VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                dai.descriptorPool = P.resolveDescPool;
                dai.descriptorSetCount = P.resolveDescCapacity;
                dai.pSetLayouts = dlayouts.data();
                if (vkAllocateDescriptorSets(device, &dai, RT.resolveDepthSets.data()) !=
                    VK_SUCCESS)
                    RT.resolveDepthSets.clear();
            }
        }
    }

    // One descriptor set per format reinterpretation. A frame's format changes
    // are bounded by its draws, but only a handful ever happen (12 on the Act 1
    // capture's one mixed surface), so the pool is small and grows only if a
    // frame needs more than the last one did.
    if (P.reinterpretPipeline != VK_NULL_HANDLE)
    {
        const uint32_t want = std::max<uint32_t>(32, plan.drawCount / 8 + 8);
        if (P.reinterpretDescPool == VK_NULL_HANDLE || P.reinterpretDescCapacity < want)
        {
            vkDestroyDescriptorPool(device, P.reinterpretDescPool, nullptr);
            P.reinterpretDescPool = VK_NULL_HANDLE;
            const VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, want};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = want;
            pi.poolSizeCount = 1;
            pi.pPoolSizes = &ps;
            if (vkCreateDescriptorPool(device, &pi, nullptr, &P.reinterpretDescPool) == VK_SUCCESS)
                P.reinterpretDescCapacity = want;
        }
        if (P.reinterpretDescPool != VK_NULL_HANDLE)
        {
            vkResetDescriptorPool(device, P.reinterpretDescPool, 0);
            std::vector<VkDescriptorSetLayout> layouts(P.reinterpretDescCapacity,
                                                       P.reinterpretSetLayout);
            RT.reinterpretSets.resize(P.reinterpretDescCapacity);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = P.reinterpretDescPool;
            ai.descriptorSetCount = P.reinterpretDescCapacity;
            ai.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(device, &ai, RT.reinterpretSets.data()) != VK_SUCCESS)
                RT.reinterpretSets.clear();
        }
    }
    // The aliasing pass's own sets: three bindings each, and a frame performs
    // one per aliasing draw (three in the Act 1 capture). Sized from the same
    // pool budget as the reinterpretation, which is the same order of magnitude.
    if (P.depthAliasPipeline != VK_NULL_HANDLE)
    {
        const uint32_t want = 16;
        if (P.depthAliasDescPool == VK_NULL_HANDLE)
        {
            const VkDescriptorPoolSize ps[2] = {
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, want * 2},
                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, want}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = want;
            pi.poolSizeCount = 2;
            pi.pPoolSizes = ps;
            vkCreateDescriptorPool(device, &pi, nullptr, &P.depthAliasDescPool);
        }
        if (P.depthAliasDescPool != VK_NULL_HANDLE)
        {
            vkResetDescriptorPool(device, P.depthAliasDescPool, 0);
            std::vector<VkDescriptorSetLayout> layouts(want, P.depthAliasSetLayout);
            RT.depthAliasSets.resize(want);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = P.depthAliasDescPool;
            ai.descriptorSetCount = want;
            ai.pSetLayouts = layouts.data();
            if (vkAllocateDescriptorSets(device, &ai, RT.depthAliasSets.data()) != VK_SUCCESS)
                RT.depthAliasSets.clear();
        }
    }

    draw::ReportResolvePlan(plan, in.report);

    // Which image serves each texture binding, and the census of what the
    // frame's bindings named, live on TextureBinder in gpu_draw_textures.
    draw::TextureBinder TB(P, TX, plan.dests, plan.depthDests, options.textureBindingsPsHash);
    TB.rtLinkEnabled = !lucent::config::flag("DRAW_NORT");
    TB.texUploadEnabled = texUploadEnabled;
    const bool listDraws = lucent::config::flag("DRAW_FRAME_LIST");
    // GEARS_DRAW_PS_CONSTS names a pixel shader whose constants to print, and
    // the printing lives inside the per-draw listing -- so asking for it ALONE
    // used to produce nothing at all, with no line saying why. It now pulls the
    // listing in for the draws it names, and only those, rather than requiring
    // GEARS_DRAW_FRAME_LIST=1 (which prints a line for every draw in the frame).
    const std::string &psConstsWant = lucent::config::text("DRAW_PS_CONSTS");
    const uint64_t psConstsHash =
        psConstsWant.empty() ? 0 : std::strtoull(psConstsWant.c_str(), nullptr, 16);
    uint32_t psConstsMatched = 0;

    // The guest's resolve rectangle and the kCopy decode are in
    // gpu_draw_resolve_decode.{h,cpp}.

    draw::UniformCache UC(AR, options.applyTextureSigns);
    draw::IndexPreparer IP;
    // THE DESCRIPTOR SETS RIDE THE SAME INPUTS. A draw's texture sets are
    // determined by the shader pair, the fetch constants behind every binding
    // and the resolve generation -- see gpu_draw_descriptors.h for why that is
    // the key and what keying on identity instead cost.
    draw::DescriptorBuilder DB(*this, P, TB, TX);
    DB.pool = pool;
    DB.ssbo = biSsbo;
    DB.stubSampler = samp;

    // WHAT THE GUEST PROGRAMMED, counted BEFORE any drop. The draw-stream
    // emitter at the end of this function records from `prepared`, so a draw
    // this loop drops -- at any of the ten `CN.Skip(...); continue` sites below
    // -- is invisible to it. That is the stated falsifier on claim C023: the
    // eight pixel shaders the oracle binds and our stream never shows could be
    // shaders the guest never asks for, OR shaders it asks for and we throw
    // away, and the prepared-level recording cannot tell those apart. This map
    // is filled at the TOP of the loop body, above every early exit, so the
    // difference between it and the prepared counts IS the set we drop.
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> rawCounts;
    static const std::string &rawStreamPath = lucent::config::text("DRAW_STREAM_RAW");
    const bool wantRawStream = !rawStreamPath.empty();

    for (const FrameDrawItem &d : in.draws)
    {
        const double stateBegin = sinceStartMs();
        const uint32_t *R = d.registers();
        if (!R)
        {
            CN.Skip(1);
            continue;
        }

        if (wantRawStream)
        {
            // Same two normalisations the prepared-level emitter applies, so
            // the two are comparable to each other AND to the oracle: resolve
            // (kCopy) draws are not IssueDraw on the Xenia side and are left
            // out, and a draw with no fragment stage records ps = 0 because
            // Xenia sets pixel_shader = nullptr there.
            const uint32_t rawMode = R[0x2208] & 0x7;
            if (rawMode != 6 /*kCopy*/)
                ++rawCounts[{d.vsHash, rawMode == 4 /*kColorDepth*/ ? d.psHash : 0}];
        }

        // Which EDRAM surface this draw targets (RB_COLOR_INFO: color_base is
        // the low 12 bits in tiles, color_format bits 16..19), and what the
        // draw is for (RB_MODECONTROL.edram_mode). Both are read before any
        // early exit so the census covers every draw, resolves included.
        const uint32_t surfaceBase = R[0x2001] & 0xFFF;
        const uint32_t edramMode = R[0x2208] & 0x7;
        CN.NoteDraw(surfaceBase, (R[0x2001] >> 16) & 0xF, edramMode);
        if (edramMode == 6 /*kCopy*/)
        {
            ResolveEvent re;
            re.drawIndex = uint32_t(&d - in.draws.data());
            re.srcSelect = R[0x2318] & 0x7;
            re.srcIsDepth = re.srcSelect >= 4; // kMaxColorRenderTargets
            static const uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
            const uint32_t info = re.srcIsDepth ? R[0x2002] : R[kColorInfo[re.srcSelect & 3]];
            re.srcBase = info & 0xFFF;
            re.srcFormat = (info >> 16) & 0xF;
            re.destBase = R[0x2319] & ~0xFFFu;
            const uint32_t copyControl = R[0x2318];
            re.colorClear = (copyControl >> 8) & 1;
            re.depthClear = (copyControl >> 9) & 1;
            re.copyCommand = (copyControl >> 20) & 3;
            re.colorClearValue = R[0x231E];
            re.depthClearValue = R[0x231D];
            re.depthBase = R[0x2002] & 0xFFF;
            re.depthFormat = (R[0x2002] >> 16) & 1;
            // PA_SC_WINDOW_OFFSET, two signed 15-bit fields. The resolve rect
            // is shifted by it exactly as geometry is, which is what makes a
            // predicated tile resolve to its own part of the destination.
            const uint32_t wo = R[0x2080];
            auto sign15 = [](uint32_t v)
            {
                v &= 0x7FFF;
                return int32_t(v) - int32_t((v & 0x4000) << 1);
            };
            if ((R[0x2205] >> 16) & 1 /*vtx_window_offset_enable*/)
            {
                re.windowX = sign15(wo);
                re.windowY = sign15(wo >> 16);
            }
            // vf0: type must be kVertex and size exactly 3 vertices x 2 floats.
            const uint32_t vf0 = R[0x4800];
            const uint32_t vf1 = R[0x4801];
            if ((vf0 & 3) == 3 && ((vf1 >> 2) & 0xFFFFFF) == 6)
            {
                const uint64_t addr = uint64_t(vf0) & ~uint64_t(3);
                if (addr + 6 * 4 <= in.guestWindowBytes)
                {
                    for (int k = 0; k < 6; ++k)
                    {
                        uint32_t raw;
                        std::memcpy(&raw, in.guestBase + addr + k * 4, 4);
                        raw = __builtin_bswap32(raw);
                        std::memcpy(&re.rect[k], &raw, 4);
                    }
                    re.haveRect = true;
                }
            }
            re.destPitch = R[0x231A] & 0x3FFF;
            re.destHeight = (R[0x231A] >> 16) & 0x3FFF;
            re.destFormat = (R[0x231B] >> 7) & 0x3F;
            re.destNumber = (R[0x231B] >> 13) & 0x7;
            {
                const uint32_t raw = (R[0x231B] >> 16) & 0x3F;
                re.destExpBias = int32_t(raw) - int32_t((raw & 0x20) << 1);
            }
            re.destEndian = R[0x231B] & 0x7;
            re.destSwap = (R[0x231B] >> 24) & 1;
            resolves.push_back(re);
        }
        // How many distinct depth surfaces the frame uses. One host depth image
        // is shared by every colour target; this is the number that says when
        // that stops being faithful.

        // Which (colour surface, depth base) pairs the frame actually uses, and
        // how many draws each. One shared host depth image is only wrong if a
        // surface's draws span more than one depth base, or two surfaces share
        // one -- this says which, instead of assuming.
        CN.NoteDepth(R[0x2001] & 0xFFF, R[0x2002] & 0xFFF);
        // A draw whose DEPTH buffer sits at the same EDRAM base as its COLOUR
        // surface. On the console that is one memory: the depth and stencil it
        // writes LAND IN the colour surface's bits, and a later colour draw
        // blending against that destination reads them. This renderer keeps a
        // colour image per base and one shared depth image, so it cannot
        // represent the aliasing at all -- named per draw because "the frame
        // has three such draws" cannot say which pass they belong to
        // (catalog #91).
        if ((R[0x2001] & 0xFFF) == (R[0x2002] & 0xFFF))
            lucent::debug("draw",
                          "diag draw {} renders colour AND depth at the"
                          " SAME EDRAM base {:#x}: on the console its depth/stencil writes"
                          " overwrite that surface's colour bits",
                          uint32_t(&d - in.draws.data()), R[0x2001] & 0xFFF);

        // A resolve is not geometry. It copies an EDRAM surface out to main
        // memory, and the primitive only selects the region; issuing it as a
        // draw paints the resolve rectangle's shader output over the surface.
        // Handled before anything else because it needs no shaders at all.
        if ((R[0x2208] & 0x7) == 6 /*kCopy*/)
        {
            // A kCopy draw is decoded in gpu_draw_resolve_decode.{h,cpp}: it is
            // a resolve, not geometry, and it needs no shaders at all.
            draw::PrepareResolveDraw(R, d, in, W, H, plan.routing, msaaModel, RT, CN, prepared);
            continue;
        }
        if (!d.vsUcode || !d.psUcode)
        {
            CN.Skip(1);
            continue;
        }

        // Resolve the persistent target before translating this draw's pixel
        // shader. A later frame may promote the target from the narrow format
        // established at boot to a mixed HDR container, and the shader's
        // guest-format clamp must be selected against the promoted image.
        static const long onlyBase = lucent::config::number("DRAW_ONLY_BASE", -1);
        if (onlyBase >= 0 && surfaceBase != uint32_t(onlyBase))
        {
            CN.Skip(0);
            continue;
        }
        const draw::DrawSampleLayout sampleLayout = draw::DeriveDrawSampleLayout(
            msaaModel ? ((R[0x2000] >> 16) & 3) : 0, P.width, P.height);
        if (sampleLayout.IsNativeMultisample() &&
            (!hasStandardSampleLocations || !has2xFramebufferSamples))
        {
            lucent::error("draw",
                          "the selected Vulkan device lacks {} required for faithful Xenos"
                          " 2X coverage",
                          !has2xFramebufferSamples ? "2X colour/depth framebuffer samples"
                                                   : "standard sample locations");
            return false;
        }
        SurfaceTarget *target = nullptr;
        if (!RT.GetSurfaceTarget(surfaceBase, sampleLayout, target))
        {
            CN.Skip(8);
            continue;
        }
        std::pair<VkRenderPass, VkRenderPass> *rp = nullptr;
        if (!RT.GetPasses(target->hostFormat, target->samples, rp))
        {
            CN.Skip(8);
            continue;
        }

        draw::ShaderXlate *vsX = nullptr, *psX = nullptr;
        VkShaderModule vsMod = VK_NULL_HANDLE, psMod = VK_NULL_HANDLE;
        // The interpolator mask (and the rest of the modification) is a property
        // of this draw's VS+PS pair and its own registers, so it is derived
        // here, per draw, before either stage is translated.
        uint64_t vsModification = 0, psModification = 0;
        {
            // Derived per draw from this draw's own registers, so unlike the
            // translation it caches behind it, this runs every time. ScopedMs
            // because the failure path takes a `continue` -- and a draw whose
            // modification cannot be derived is exactly the one worth counting.
            ScopedMs modifyTime(msModify);
            if (!draw::DeriveShaderModifications(R, d.vsUcode, d.vsUcodeSize, d.vsHash, d.psUcode,
                                                 d.psUcodeSize, d.psHash, vsModification,
                                                 psModification))
            {
                CN.Skip(2);
                continue;
            }
        }
        // Set before ANY texture binding of this draw is resolved: TextureBinder::SelectView
        // runs while the descriptor sets are built, which is earlier than the
        // PreparedDraw is filled in. Recording it later attributed every binding
        // to the PREVIOUS draw's shader -- and sent me disassembling a shader
        // with no texture fetch in it at all.
        TB.currentPsHash = d.psHash;
        VkDescriptorSetLayout vsTexLayout = VK_NULL_HANDLE, psTexLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
        {
            // The cache LOOKUPS, not the translation: msTranslate and msPipeline
            // already measure the work done on a miss, and both are ~0 once the
            // frame is warm. What is left here is what a hit costs, times the
            // number of draws -- which is the quantity a per-draw cache has to
            // justify and which nothing was measuring.
            ScopedMs lookupTime(msShaderLookup);
            // Does this draw need the clamp its render target used to do for it?
            // Only when the guest's format is fixed-point AND the host image was
            // widened to a float container to hold a reinterpreted surface.
            draw::ClampMode clampMode = draw::ClampMode::kRgba;
            bool clampPs = false;
            {
                const GuestClamp want = GuestColorFormatClamp((R[0x2001] >> 16) & 0xF);
                const bool hostIsFloat = target->hostFormat == VK_FORMAT_R16G16B16A16_SFLOAT ||
                                         target->hostFormat == VK_FORMAT_R32G32_SFLOAT ||
                                         target->hostFormat == VK_FORMAT_R16G16_SFLOAT;
                if (hostIsFloat && want != GuestClamp::kNone)
                {
                    clampPs = true;
                    clampMode = want == GuestClamp::kRgba ? draw::ClampMode::kRgba
                                                          : draw::ClampMode::kAlphaOnly;
                }
            }
            if (!SC.GetShader(true, d.vsUcode, d.vsUcodeSize, d.vsHash, vsModification, false,
                              draw::ClampMode::kRgba, vsX, vsMod) ||
                !SC.GetShader(false, d.psUcode, d.psUcodeSize, d.psHash, psModification, clampPs,
                              clampMode, psX, psMod))
            {
                CN.Skip(2);
                continue;
            }
            if (!PC.GetPipeLayout(*vsX, *psX, vsTexLayout, psTexLayout, pipeLayout))
            {
                CN.Skip(3);
                continue;
            }
        }
        OutputMergerState om;
        om.colorMask = R[0x2104];
        om.blend0 = R[0x2201];
        om.depthControl = R[0x2200];
        om.stencilRefMask = R[0x210D];
        om.stencilRefMaskBf = R[0x210C];
        om.suScModeCntl = R[0x2205];
        om.polygonal = draw::IsPrimitivePolygonal(R);
        // PA_CL_CLIP_CNTL.clip_disable (bit 16): the guest wants no near/far
        // clipping. Xenia gates the same state on the same device feature, so a
        // device without depthClamp renders as this one did before -- with the
        // primitives whose Z is a hair outside [0,1] clipped away.
        om.depthClamp = hasDepthClamp && ((R[0x2204] >> 16) & 1) != 0;
        VkShaderModule gsMod = VK_NULL_HANDLE;
        if (d.primType == 1 /*kPointList*/)
            PC.GetPointGeomShader(vsModification, psModification, gsMod);
        else if (d.primType == 8 /*kRectangleList*/)
        {
            ++PC.rectDraws;
            if (PC.GetRectGeomShader(vsModification, gsMod))
                ++PC.rectDrawsExpanded;
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
        static const bool depthOnlyRunsPs = lucent::config::flag("DRAW_DEPTHONLY_PS");
        bool pixelShaderUsed = edramMode == 4 /*kColorDepth*/ || depthOnlyRunsPs;
        // AND THE OTHER TWO CONDITIONS XENIA APPLIES. edram_mode is only the
        // middle one of three; see draw::ClassifyDraw. Measured at the same
        // guest frame as the console, on the title screen, the mode test alone
        // put 50 of 55 draws of one vertex shader through a pixel shader where
        // the console put 2 -- the other 48 are a Z-prepass whose colour writes
        // are entirely masked off by RB_COLOR_MASK.
        //
        // GEARS_DRAW_MODE_ONLY=1 restores the mode-only test: a DIAGNOSTIC
        // control arm for A/B-ing exactly this, never a fix.
        static const bool modeOnly = lucent::config::flag("DRAW_MODE_ONLY");
        if (!modeOnly && !depthOnlyRunsPs)
        {
            draw::DrawClassification dc;
            if (!draw::ClassifyDraw(R, d.psUcode, d.psUcodeSize, d.psHash, dc))
            {
                // UNDECIDED, not "no". Counted and skipped rather than guessed:
                // treating a shader we could not analyse as depth-only would
                // silently drop its colour and look exactly like the console's
                // own Z-prepass.
                ++CN.drawsClassifyFailed;
            }
            else if (!dc.rasterisationDone)
            {
                // Xenia SKIPS these entirely -- the draw covers nothing.
                ++CN.drawsNoRasterisation;
                CN.Skip(9);
                continue;
            }
            else if (!dc.pixelShaderNeeded)
            {
                pixelShaderUsed = false;
            }
        }
        if (!pixelShaderUsed)
            ++CN.drawsNoPixelShader;
        VkPipeline pipe = VK_NULL_HANDLE;
        if (!PC.GetPipeline(vsMod, pixelShaderUsed ? psMod : VK_NULL_HANDLE, gsMod, d.primType, om,
                            rp->first, target->samples, pipeLayout, pipe))
        {
            CN.Skip(3);
            continue;
        }

        // The five per-draw constant blocks and the cache that keeps consecutive
        // draws from repacking identical bytes live in gpu_draw_uniforms.{h,cpp}.
        const double uniformsBegin = sinceStartMs();
        msState += uniformsBegin - stateBegin;
        const draw::UniformCache::Result ur = UC.Update(R, d, *vsX, *psX);
        if (ur == draw::UniformCache::Result::kFailed)
        {
            CN.Skip(4);
            continue;
        }
        VkDescriptorBufferInfo biSys = UC.biSys, biFvs = UC.biFvs;
        VkDescriptorBufferInfo biFps = UC.biFps, biBl = UC.biBl;
        VkDescriptorBufferInfo biFetch = UC.biFetch;
        msUniforms += sinceStartMs() - uniformsBegin;
        const double indexBegin = sinceStartMs();

        // Guest index conversion and quad expansion live in gpu_draw_indices.
        draw::PreparedIndices idx;
        switch (IP.Prepare(AR, in, d, idx))
        {
        case draw::IndexResult::kEmptyQuad:
            CN.Skip(7);
            continue;
        case draw::IndexResult::kArenaFull:
            CN.Skip(5);
            continue;
        case draw::IndexResult::kOk:
            break;
        }
        const VkBuffer ibuf = idx.buffer;
        const VkDeviceSize ibufOffset = idx.offset;
        const uint32_t drawCount = idx.count;
        const bool drawIndexed = idx.indexed;

        msIndex += sinceStartMs() - indexBegin;

        // Everything from here to the end of the draw's body: descriptor
        // allocation and update, the PreparedDraw itself, the viewport
        // derivation and the census. It was declared and never accumulated, so
        // the breakdown reported ~2 ms of a ~36 ms draw loop and the remaining
        // 18 ms had no name at all -- which reads as "the loop is cheap" rather
        // than "the loop is unmeasured". DB.msAlloc/DB.msUpdate are inside it.
        ScopedMs recordTime(msRecord);

        // The draw's four descriptor sets, and the per-frame cache that lets most
        // draws reuse the expensive two, are in gpu_draw_descriptors.{h,cpp}.
        VkDescriptorSet sets[4] = {};
        if (!DB.Build(R, d, RT.resolveGeneration, *vsX, *psX, vsTexLayout, psTexLayout, UC, sets))
        {
            CN.Skip(6);
            continue;
        }

        // Building the PreparedDraw and deriving this draw's viewport/scissor.
        // Runs to the end of the body, which has no further `continue`.
        const double prepareBegin = sinceStartMs();
        PreparedDraw pd{};
        pd.pipeline = pipe;
        pd.layout = pipeLayout;
        pd.sets[0] = sets[0];
        pd.sets[1] = sets[1];
        pd.sets[2] = sets[2];
        pd.sets[3] = sets[3];
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
        const draw::DepthBias depthBias = draw::DeriveDepthBias(R, om.polygonal);
        pd.depthBiasConstant = depthBias.constantFactor;
        pd.depthBiasSlope = depthBias.slopeFactor;
        // GEARS_DRAW_NODEPTHBIAS=1 is the control arm for the formerly missing
        // state. It is never a fallback or fix: zeroing guest bias should make
        // a shadow-map regression reappear if this mechanism matters.
        if (!options.applyDepthBias)
            pd.depthBiasConstant = pd.depthBiasSlope = 0.0f;
        pd.depthBase = R[0x2002] & 0xFFF;
        // WHICH HOST DEPTH IMAGE THIS DRAW RENDERS INTO -- a separate question
        // from which base the guest named, and the two must not be conflated.
        // `depthBase` is also what the EDRAM colour/depth ALIASING pass
        // triggers on (`pd.depthBase == pd.surfaceBase`), so forcing it to zero
        // to select a shared image silently switched that pass off and blew the
        // shadow-mask pass out to 644,645 shadowed pixels against 120,984.
        // Measured, not reasoned: that was this change's own first attempt.
        //
        // ONE IMAGE PER BASE IS ON BY DEFAULT -- see SplitDepthEnabled() in
        // gpu_draw_formats.h for the measurement and the one open regression.
        // It was off because "switching it on makes this frame's masks worse",
        // and that comparison was across DIFFERENT game moments of a pass whose
        // content varies between them; held to ONE moment with a repeat-run
        // noise floor, the split wins nine passes of ten including the front
        // buffer.
        pd.depthTargetBase = SplitDepthEnabled() ? pd.depthBase : 0u;
        pd.guestDepthBase = pd.depthBase;
        // The depth FORMAT at this draw, for the aliasing pass: the same bit
        // the resolve path reads, but a geometry draw needs its own copy --
        // resolveDepthIsFloat24 is only filled for kCopy draws.
        pd.resolveDepthIsFloat24 = ((R[0x2002] >> 16) & 1) == 1;
        pd.depthControl = om.depthControl;
        pd.clipDisable = om.depthClamp;
        pd.stencilRefMask = om.stencilRefMask;
        pd.blend0 = om.blend0;
        pd.colorFormat = (R[0x2001] >> 16) & 0xF;
        pd.colorExpBias = int32_t((R[0x2001] >> 20) & 0x3F);
        if (pd.colorExpBias & 0x20)
            pd.colorExpBias -= 64;
        pd.clipCntl = R[0x2204];
        pd.suScModeCntl = R[0x2205];
        pd.vteCntl = R[0x2206];
        pd.windowOffset = R[0x2080];
        pd.surfaceInfo = R[0x2000];
        pd.sampleLayout = sampleLayout;
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
            static const bool fixedVp = lucent::config::flag("DRAW_FIXEDVP");
            if (fixedVp)
            {
                gv.x = gv.y = gv.scissorX = gv.scissorY = 0;
                gv.w = gv.scissorW = W;
                gv.h = gv.scissorH = H;
                gv.zMin = 0.0f;
                gv.zMax = 1.0f;
            }
            // A std::format plus a map insert PER DRAW, feeding a census that is
            // only ever printed under `in.report` -- so on the 59 frames out of
            // 60 that print nothing it was building strings for nobody. Measured
            // at 3.6 ms of a 39.4 ms draw loop on gameplay frames, which is 9%
            // of the render spent on a diagnostic that produced no output.
            //
            // Gated on the same flag that prints it, so the report frames are
            // byte-for-byte what they were and only the silent frames get
            // cheaper. Still measured, because a diagnostic whose cost is
            // assumed is how this one survived.
            if (in.report || ab.Arm())
            {
                ScopedMs censusTime(msCensus);
                ++CN.viewportCensus[std::format("{},{} {}x{} scissor {},{} {}x{}", gv.x, gv.y, gv.w,
                                                gv.h, gv.scissorX, gv.scissorY, gv.scissorW,
                                                gv.scissorH)];
            }
            // THE VIEWPORT IS BOUNDED BY THE DEVICE, NOT BY THE TARGET.
            //
            // It used to be clamped to the render target, which looks obviously
            // right and is wrong: with PA_CL_CLIP_CNTL.clip_disable the guest
            // wants no clipping, and the emulation of that is a viewport larger
            // than the target (Xenia returns up to 8192) with the vertex shader
            // rescaling into it through the ndc_scale it hands the same
            // function. Clamping one side of that pair and not the other
            // shrinks the draw by the ratio between them.
            //
            // MEASURED, on walk_gameplay.gfr: draw 640 is a rectangle list with
            // window-space vertices spanning (-0.5,-0.5)..(639.5,359.5) -- the
            // top-left quadrant, 230,400 px -- and with the clamp it wrote depth
            // and stencil over exactly 3,200 px in a 100x32 corner. 640 * 1280/
            // 8192 = 100 and 360 * 720/8192 = 31.6. That draw is the shadow
            // mask's depth+stencil fill (catalog #91), so the whole pass was
            // built on a 1/72 of the coverage the console has.
            //
            // The scissor still IS clamped to the target: it is what keeps the
            // large viewport from writing outside the image, which is exactly
            // the division of labour Xenia's viewport code assumes.
            // `R` in this function is the REGISTER FILE; the renderer is this.
            const uint32_t vpMaxW = maxViewportDim[0], vpMaxH = maxViewportDim[1];
            if (gv.w > vpMaxW || gv.h > vpMaxH)
            {
                // A clamp here DOES shrink the draw, for the same reason the
                // old one did -- so it is said out loud rather than applied
                // quietly. 8192 is the largest Xenia's viewport code returns and
                // every device this has run on allows 16384.
                lucent::warn("draw",
                             "diag draw {}: the guest wants a {}x{} host"
                             " viewport (clipping disabled) and this device allows only"
                             " {}x{}. It is clamped, and this draw's geometry WILL be"
                             " smaller than the console's by that ratio",
                             pd.diagIndex, gv.w, gv.h, vpMaxW, vpMaxH);
            }
            // INTO THE SAMPLE GRID. The viewport and scissor the guest
            // programmed are in PIXELS of ITS surface; the target is EDRAM,
            // which is samples. Multiplying by this draw's own sample scale is
            // the whole of the conversion, and it is what makes a 4X 640x360
            // fill and a 1X 1280x720 composite cover the same 1280x720 samples
            // -- the same bytes the console gives them. Both scales are 1 with
            // the model off, so this line is the identity there.
            const uint32_t sx = msaaModel ? pd.sampleLayout.viewportScaleX : 1u;
            const uint32_t sy = msaaModel ? pd.sampleLayout.viewportScaleY : 1u;
            pd.viewport.x = float(gv.x * sx);
            pd.viewport.y = float(gv.y * sy);
            pd.viewport.width = float(std::min(gv.w * sx, vpMaxW));
            pd.viewport.height = float(std::min(gv.h * sy, vpMaxH));
            pd.viewport.minDepth = gv.zMin;
            pd.viewport.maxDepth = gv.zMax;
            const uint32_t scx = gv.scissorX * sx, scy = gv.scissorY * sy;

            const uint32_t targetW = msaaModel ? pd.sampleLayout.imageWidth : SW;
            const uint32_t targetH = msaaModel ? pd.sampleLayout.imageHeight : SH;
            pd.scissor.offset = {int32_t(std::min(scx, targetW)), int32_t(std::min(scy, targetH))};
            pd.scissor.extent = {std::min(gv.scissorW * sx, targetW - std::min(scx, targetW)),
                                 std::min(gv.scissorH * sy, targetH - std::min(scy, targetH))};
        }
        draw::DumpVertices(R, in, d, *vsX, *psX, issued, pd.diagIndex, pd.vsHash, d.indexCount,
                           &UC.sysc, &UC.fVs);
        draw::DumpVsConstants(*vsX, UC, d.vsHash, issued, pd.diagIndex, R);
        // What this draw fetches, and whether the mirror covers it, is in
        // gpu_draw_vertexfetch.{h,cpp}.
        draw::CollectFetchRanges(R, in, *vsX, *psX, CN, fetchRanges);
        const bool psConstsHit = psConstsHash != 0 && d.psHash == psConstsHash;
        if (psConstsHit)
            ++psConstsMatched;
        if (listDraws || psConstsHit)
            draw::ListDraw(R, d, in, *vsX, *psX, UC, issued);
        prepared.push_back(pd);
        msPrepare += sinceStartMs() - prepareBegin;
        ++issued;
    }

    // A HASH THAT MATCHED NOTHING MUST NOT LOOK LIKE A SHADER WITH NO
    // CONSTANTS. Both print nothing, and the difference is "you asked about a
    // shader this frame never ran" versus "the constants are all zero".
    if (psConstsHash != 0 && psConstsMatched == 0)
        lucent::warn("draw",
                     "GEARS_DRAW_PS_CONSTS={}: NO draw in this frame"
                     " used that pixel shader, so nothing was printed about it. The"
                     " frame issued {} draws",
                     psConstsWant, issued);

    // The EDRAM-tiling collapse lives in gpu_draw_untile.{h,cpp}; the reasoning
    // for it, and for what it refuses to do, is on that header.
    if (untileThisFrame)
        draw::CollapseEdramTiling(prepared, issued, msaaModel, in.report);

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
        for (auto &r : fetchRanges)
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
                std::memcpy(static_cast<uint8_t *>(P.ssboMapped) + begin, in.guestBase + begin,
                            size_t(end - begin));
                uploadedBytesSsbo += end - begin;
                ++spans;
            }
            i = j;
        }
        accumulate(msSsboUpload, tUpload);
        if (in.report)
            lucent::info("draw",
                         "frame guest-memory upload: {} KiB in {} spans"
                         " (mirror spans {} MiB)",
                         uploadedBytesSsbo / 1024, spans, in.guestPhysicalMirrorBytes >> 20);
    }

    // --- readback buffer -------------------------------------------------
    const VkDeviceSize rbBytes = VkDeviceSize(SW) * SH * 4;
    if (P.readback == VK_NULL_HANDLE)
    {
        if (!MakeBuffer(rbBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, P.readback, P.readbackMem,
                        /*wantCached=*/true))
            return false;
        P.readbackBytes = rbBytes;
        VK_CHECK(vkMapMemory(device, P.readbackMem, 0, rbBytes, 0, &P.readbackMapped));
    }
    VkBuffer readback = P.readback;

    // --- command buffer: clear once, draw all in order -------------------
    VkCommandPool &cmdPool = P.cmdPool;
    if (cmdPool == VK_NULL_HANDLE)
    {
        VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        ci.queueFamilyIndex = queueFamily;
        VK_CHECK(vkCreateCommandPool(device, &ci, nullptr, &cmdPool));
    }
    VkCommandBuffer &cmd = P.cmd;
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
        {stub2D.image, 1u, false}, {stub3D.image, 1u, false}, {stubCube.image, 6u, false}};
    for (const auto &[destBase, rt] : P.resolveTargets)
        initialClears.emplace_back(rt.image, 1u, true);
    for (const auto &[img, layers, isRt] : initialClears)
    {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = img;
        toDst.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDst);
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
        toRead.image = img;
        toRead.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toRead);
    }

    // Guest textures: staging buffer -> image, once each, before any draw.
    for (const draw::TextureUploader::PendingUpload &u : TX.uploads)
    {
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, u.mipLevels, 0,
                                      u.regions.front().imageSubresource.layerCount};
        VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toDst.srcAccessMask = 0;
        toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toDst.image = u.image;
        toDst.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toDst);
        // Each decoded mip is tightly packed at its recorded staging offset.
        vkCmdCopyBufferToImage(cmd, u.staging, u.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               uint32_t(u.regions.size()), u.regions.data());
        VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = u.image;
        toRead.subresourceRange = range;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &toRead);
    }

    // The depth clear value the guest programmed, taken from the frame's own
    // clear-carrying resolves. If the frame contains none, there is nothing to
    // take and the previous host constant stands -- reported, not hidden, so a
    // frame that clears differently is never silently given someone else's value.
    float guestDepthClear = 1.0f;
    bool haveGuestDepthClear = false;
    for (const ResolveEvent &re : resolves)
    {
        if (!re.depthClear)
            continue;
        // RB_DEPTH_CLEAR packs depth in bits 8..31 and stencil in bits 0..7.
        const uint32_t d24 = re.depthClearValue >> 8;
        const float v =
            re.depthFormat == 1 /*kD24FS8*/ ? Depth20e4To32(d24) : DepthUnorm24To32(d24);
        if (haveGuestDepthClear && v != guestDepthClear)
            lucent::warn("draw",
                         "frame programs two different depth clears"
                         " ({} and {}); using the first",
                         guestDepthClear, v);
        else
            guestDepthClear = v;
        haveGuestDepthClear = true;
    }
    if (in.report)
        lucent::info("draw", "frame depth clear: {} ({})", guestDepthClear,
                     haveGuestDepthClear ? "the guest's own, from RB_DEPTH_CLEAR"
                                         : "HOST DEFAULT -- this frame programs no depth clear");

    // The colour clear. Like the depth clear, the guest's rides on a copy draw:
    // RB_COPY_CONTROL bit 8 is color_clear_enable and the value is in
    // RB_COLOR_CLEAR / RB_COLOR_CLEAR_LO.
    //
    // Only the ZERO case is honoured, deliberately. Every colour clear observed
    // across every captured run of this title is 0x00000000 -- 478 of them, not
    // one non-zero -- so a per-format unpack of that register would produce
    // identical output whether it were right or wrong, and shipping it would be
    // shipping something this data cannot test. A non-zero value is reported and
    // falls back rather than being decoded on a guess.
    //
    // The dark slate it replaces was a DIAGNOSTIC ("any lit pixel is guest
    // geometry"), not the guest's colour, and it is not black: it lifted every
    // surface -- including the HDR one the tonemap samples -- off zero.
    // GEARS_DRAW_SLATE_CLEAR=1 brings it back as a control arm.
    float guestColorClear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool haveGuestColorClear = false;
    uint32_t unverifiableColorClears = 0;
    for (const ResolveEvent &re : resolves)
    {
        if (!re.colorClear)
            continue;
        if (re.colorClearValue != 0)
        {
            ++unverifiableColorClears;
            continue;
        }
        haveGuestColorClear = true;
    }
    if (unverifiableColorClears)
        lucent::warn("draw",
                     "frame programs {} NON-ZERO colour clears; this build"
                     " only honours a zero clear, because no captured frame of this title"
                     " has ever programmed another value and the per-format unpack of"
                     " RB_COLOR_CLEAR is therefore untestable. Falling back.",
                     unverifiableColorClears);
    static const bool slateClear = lucent::config::flag("DRAW_SLATE_CLEAR");
    const bool useGuestColorClear = haveGuestColorClear && !unverifiableColorClears && !slateClear;
    if (in.report)
        lucent::info("draw", "frame colour clear: {}",
                     useGuestColorClear ? "the guest's own (0x00000000)"
                                        : "HOST DIAGNOSTIC SLATE -- not the guest's colour");

    VkClearValue clears[2]{};
    clears[0].color.float32[0] = useGuestColorClear ? 0.0f : 0.05f;
    clears[0].color.float32[1] = useGuestColorClear ? 0.0f : 0.05f;
    clears[0].color.float32[2] = useGuestColorClear ? 0.0f : 0.08f;
    clears[0].color.float32[3] = useGuestColorClear ? guestColorClear[3] : 1.0f;
    // The depth clear is the GUEST'S OWN. On Xenos a clear is not a packet of
    // its own: it rides on a resolve (a kCopy draw) whose RB_COPY_CONTROL sets
    // depth_clear_enable, with the value in RB_DEPTH_CLEAR -- so the frame's own
    // copy draws carry it, and they are already decoded above.
    //
    // This was a host constant of 1.0, and that was catalog #31: 390 of this
    // frame's world draws are reverse-Z (PA_CL_VPORT_ZSCALE -1, ZOFFSET 1)
    // testing GEQUAL, so a 1.0 clear rejects every fragment. The guest programs
    // 0x00000000. Taking it from the guest rather than swapping 1.0 for 0.0 is
    // the difference between a fix and a second magic number: a frame that
    // clears to something else now gets what it asked for.
    //
    // GEARS_DRAW_DEPTH_CLEAR=<float> remains as an override, a control arm only.
    {
        const std::string &dc = lucent::config::text("DRAW_DEPTH_CLEAR");
        clears[1].depthStencil = {dc.empty() ? guestDepthClear : float(std::atof(dc.c_str())), 0};
    }
    // The mid-render probes -- GEARS_DRAW_FRAME_STEP's checkpoint images and
    // GEARS_DRAW_PIXEL_TRACE's per-draw texel -- live in gpu_draw_probe.{h,cpp},
    // where each one's recording half sits next to the half that reports it.
    // The probes read the render targets, which are the sample grid.
    draw::FrameProbe PB(*this, SW, SH);
    PB.Build(prepared.size(), rbBytes);

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
    // The two resolve dispatches live on the render-target cache, in
    // gpu_draw_resolve.cpp: it already owns the descriptor sets they consume
    // and the counters that report a resolve it could not serve.

    // Per-draw pipeline statistics and the diagnostic table built on them live
    // in gpu_draw_probe.{h,cpp}, with the frame's other instruments.
    draw::DrawStats DS(*this);
    DS.Build(cmd, prepared.size());

    // Every surface starts the frame un-begun, so the first draw into each one
    // CLEARS it and every draw after LOADS -- which is how the console's
    // predicated tiles accumulate into one host image per surface.
    // storageFormat goes with begunThisFrame: a surface whose first use this
    // frame CLEARS it holds no format's bits yet, so the first draw's format
    // defines the contents and needs no conversion.
    for (auto &[k, s] : P.surfaceTargets)
    {
        s.begunThisFrame = false;
        s.drawsThisFrame = 0;
        s.storageFormat = UINT32_MAX;
    }
    for (auto &[k, s] : P.surfaceTargets2x)
    {
        s.begunThisFrame = false;
        s.drawsThisFrame = 0;
        s.storageFormat = UINT32_MAX;
    }
    for (auto &[k, r] : P.resolveTargets)
        r.copies = 0;
    // Same rule for the depth targets: the frame's first use of each one
    // clears it, as the frame's first pass on a surface clears the colour.
    for (auto &[k, d] : P.depthTargets)
        d.usedThisFrame = false;
    for (auto &[k, d] : P.depthTargets2x)
        d.usedThisFrame = false;

    // WHERE A RESOLVE'S PIXELS GO, PER RESOLVE RATHER THAN PER DESTINATION.
    //
    // GEARS_DRAW_RESOLVE_DUMP writes each destination ONCE, after the frame, so
    // an address resolved six times is reported only in its LAST state. The
    // oracle probes per resolve (Xenia fork, GEARS_PROBE_AFTER_RESOLVE), and
    // catalog #79 measures that a frame's late resolves overwrite what its early
    // ones wrote -- so per-target against per-resolve compares two different
    // moments while looking like a like-for-like comparison. Of this frame's
    // eighteen resolves exactly two were comparable.
    //
    // GEARS_DRAW_RESOLVE_DUMP_EACH=1 snapshots the destination immediately after
    // every resolve executes, numbered in the same order the oracle's IssueCopy
    // log numbers them, so resolve N pairs with resolve N.
    struct ResolveDump
    {
        uint32_t base, w, h;
        // The pass's STRUCTURAL IDENTITY -- which EDRAM surface it copies out
        // of, and the destination's guest dimensions. This, not the destination
        // ADDRESS, is what pairs a pass with the console's: the title's physical
        // allocations land in different places in the two emulators (a paired
        // gameplay capture had all seven of our destinations near 0x0Cxxxxxx and
        // all eight of the console's near 0x13xxxxxx), so an address join pairs
        // nothing. These come from the guest's own registers, so both sides
        // necessarily agree on them.
        uint32_t sourceBase, destPitch, destHeight, guestFormat;
        bool isDepth;
        VkBuffer buf;
        VkDeviceMemory mem;
        uint32_t ordinal; // UINT32_MAX for the post-frame per-target dump
        uint32_t drawIndex;
    };
    std::vector<ResolveDump> resolveDumps;
    // Capture artifacts belong only to the reported frame. A COUNT=N warm run
    // renders N-1 unreported frames first; dumping those too produces hundreds
    // of files with the same structural identities and makes the comparer pair
    // a frame against an accidental multi-frame corpus.
    const bool dumpEachResolve = gears::ShouldCaptureFrameArtifact(
        in.report, lucent::config::flag("DRAW_RESOLVE_DUMP_EACH"));
    // PPM snapshots clamp HDR to [0,1] and cannot answer whether bloom crossed
    // 1.0. The opt-in exact half-floats are the instrument payload for that.
    const bool dumpResolveFloat = lucent::config::flag("DRAW_RESOLVE_DUMP_FLOAT");
    uint32_t resolveOrdinal = 0;
    // Records a copy of `r`'s image into a fresh readback buffer. Returns false
    // when it could not, and the caller COUNTS those -- a resolve missing from
    // the output and a resolve that wrote nothing must not look the same.
    uint32_t resolveSnapshotsFailed = 0, resolveSnapshotsUnwritten = 0;
    // `sourceBase` and `guestFormat` are THIS COPY'S, from the draw's own
    // registers -- never the destination ResolveTarget's, which holds whatever
    // the FIRST copy to that address declared. Destination 0xbdf0000 of the Act
    // 1 frame is written by five copies: one reads EDRAM 0x400 as
    // k_16_16_16_16_FLOAT and four read 0x2d0 as k_2_10_10_10 or
    // k_16_16_16_16_FLOAT. Keyed from the target, all five were named
    // `srcC400 f32`, so four of them paired with nothing on the console's side
    // and were read as passes the console executes and we do not (catalog #90).
    auto snapshotResolveTarget = [&](const draw::ResolveTarget &r, uint32_t ordinal,
                                     uint32_t drawIndex, uint32_t sourceBase,
                                     uint32_t guestFormat) -> bool
    {
        if (r.image == VK_NULL_HANDLE || r.width == 0 || r.imageHeight == 0)
            return false;
        // A destination the dispatch declined to write (a degenerate rectangle,
        // an offset past the image) is still in UNDEFINED layout, and the
        // barrier below claims it is in SHADER_READ_ONLY. Counted, not read.
        if (!r.everWritten)
        {
            ++resolveSnapshotsUnwritten;
            return true; // not a failure of the instrument; nothing was written
        }
        const uint32_t bpp = r.isDepth ? 4u : 8u;
        const VkDeviceSize bytes = VkDeviceSize(r.width) * r.imageHeight * bpp;
        VkBuffer b = VK_NULL_HANDLE;
        VkDeviceMemory m = VK_NULL_HANDLE;
        if (!MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b, m, true))
            return false;
        // The destination image sits in SHADER_READ_ONLY between resolves, which
        // is the layout the post passes sample it in.
        VkImageMemoryBarrier tb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        tb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        tb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        tb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        tb.srcQueueFamilyIndex = tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tb.image = r.image;
        tb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &tb);
        VkBufferImageCopy rg{};
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {r.width, r.imageHeight, 1};
        vkCmdCopyImageToBuffer(cmd, r.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &rg);
        VkImageMemoryBarrier rb2 = tb;
        rb2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        rb2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        rb2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        rb2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &rb2);
        resolveDumps.push_back({r.base, r.width, r.imageHeight, sourceBase, r.pitch, r.height,
                                guestFormat, r.isDepth, b, m, ordinal, drawIndex});
        return true;
    };

    // GEARS_DRAW_ONLY=<index>: emit only that one draw, over the clear colour.
    // A DIAGNOSTIC control arm: it shows what a single draw's shader produces
    // without anything before it having painted the target, which is the only
    // way to tell "this draw contributes nothing" from "something later
    // overwrote it".
    // The index is the ISSUED one -- how many draws this renderer has emitted
    // before it -- NOT the `draw` column of the diag table, which is the guest's
    // index and is larger wherever draws are dropped or collapsed. Passing a
    // guest index matches nothing and renders an empty frame, which reads as
    // "that draw contributes nothing" and cost this project several iterations
    // of a phantom contradiction. It is reported below either way.
    //
    // And what it shows is that draw's shader OVER THE CLEAR, with nothing
    // before it: a draw that samples a resolve target or a rendered texture has
    // no inputs in this arm and will produce black no matter how correct it is.
    // "Renders nothing under DRAW_ONLY" is therefore NOT evidence that a draw
    // writes nothing.
    const long onlyDraw = lucent::config::number("DRAW_ONLY", -1);
    uint32_t onlyDrawMatched = 0;
    uint32_t drawn = 0, segments = 0, surfaceSwitches = 0, resolvesDone = 0;
    uint32_t depthClearsDone = 0;
    static const bool noAlias = lucent::config::flag("DRAW_NOALIAS");
    uint32_t depthResolvesDone = 0, depthResolvesSkipped = 0;
    uint32_t depthResolvesFloat24 = 0;
    // The surface a render pass is currently open on, if any.
    bool inPass = false;
    const bool passLog = lucent::config::flag("DRAW_PASS_LOG");
    // The prepared index of the last draw actually ISSUED. The probes sample at
    // the top of the next iteration, so this -- not the current entry, and not
    // the draw COUNT -- is the draw whose output they are looking at. The draw
    // count indexes the wrong thing because `prepared` also holds resolves.
    uint32_t lastIssuedPrep = UINT32_MAX;
    uint32_t openSurface = 0;
    // The DEPTH base the open pass is rendering into. A pass is split on this
    // as well as on the colour surface: they are two attachments of one
    // framebuffer, and the guest changes them independently.
    uint32_t openDepthBase = UINT32_MAX;
    VkSampleCountFlagBits openSamples = VK_SAMPLE_COUNT_1_BIT;
    SurfaceTarget *openTarget = nullptr;
    // The last surface a pass was opened on, which -- unlike openTarget -- SURVIVES
    // endPass(). A checkpoint asked for immediately after a resolve has no pass
    // open, and using openTarget there dropped the checkpoint silently: exactly the
    // draws at a pass boundary, which is where the post chain's defects live. The
    // colour image sits in TRANSFER_SRC_OPTIMAL either way, so reading it closed is
    // as valid as reading it open.
    uint32_t lastSurface = 0;
    SurfaceTarget *lastTarget = nullptr;

    auto endPass = [&]()
    {
        if (!inPass)
            return;
        vkCmdEndRenderPass(cmd);
        inPass = false;
        openTarget = nullptr;
    };
    // Opens a pass on `key`'s target, clearing it if this frame has not touched
    // it yet and loading it otherwise.
    auto beginPassOn = [&](uint32_t base, uint32_t depthBase,
                           const draw::DrawSampleLayout &layout) -> bool
    {
        SurfaceTarget *t = nullptr;
        if (!RT.GetSurfaceTarget(base, layout, t))
            return false;
        std::pair<VkRenderPass, VkRenderPass> *rp = nullptr;
        if (!RT.GetPasses(t->hostFormat, t->samples, rp))
            return false;
        // Binds the depth target for this base as well as naming it, so the
        // mid-frame clear, the resolves and the probes all work on the depth
        // buffer this pass is actually rendering into (catalog #91).
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (!RT.GetFramebuffer(*t, depthBase, fb))
            return false;
        // A DEPTH TARGET'S FIRST USE THIS FRAME, when the pass taken is the
        // LOAD one. The CLEAR render pass clears both attachments and is chosen
        // by the COLOUR surface's first use; with one depth image per base the
        // two no longer coincide, and a load pass over a depth image that has
        // never been written this frame would both read undefined contents and
        // declare a layout it has never been in. Cleared explicitly here to the
        // same values that pass would have used, which keeps the single-base
        // case exactly as it was.
        DepthTarget *dt = nullptr;
        if (RT.GetDepthTarget(depthBase, layout, dt) && dt && !dt->usedThisFrame &&
            t->begunThisFrame)
        {
            VkImageSubresourceRange dr{kDepthAspects, 0, 1, 0, 1};
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = dt->image;
            b.subresourceRange = dr;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            vkCmdClearDepthStencilImage(cmd, dt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &clears[1].depthStencil, 1, &dr);
            VkImageMemoryBarrier r{b};
            r.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            r.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            r.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            r.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0,
                                 nullptr, 1, &r);
        }
        if (dt)
            dt->usedThisFrame = true;
        VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        bi.renderPass = t->begunThisFrame ? rp->second : rp->first;
        bi.framebuffer = fb;
        bi.renderArea = {{0, 0}, {t->width, t->height}};
        bi.clearValueCount = t->begunThisFrame ? 0u : 2u;
        bi.pClearValues = t->begunThisFrame ? nullptr : clears;
        // GEARS_DRAW_PASS_LOG=1: every pass begin, with what it was begun ON.
        // Two draws in the defect frame change the colour surface while their
        // colour mask is zero, and after four other mechanisms were eliminated
        // the pass begin is the only event still bound to exactly those draws
        // (catalog #62). "Which pass, which framebuffer, clear or load" is not
        // derivable from any existing line.
        if (passLog)
            lucent::info("draw",
                         "  pass begin at draw {}: surface {:#x} host"
                         " format {} framebuffer {} -- {} pass{}",
                         drawn, base, uint32_t(t->hostFormat), (void *)fb,
                         t->begunThisFrame ? "LOAD" : "CLEAR",
                         t->begunThisFrame ? "" : " (first use this frame)");
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        t->begunThisFrame = true;
        inPass = true;
        openSurface = base;
        openDepthBase = depthBase;
        openSamples = t->samples;
        openTarget = t;
        lastSurface = base;
        lastTarget = t;
        ++segments;
        return true;
    };

    for (const PreparedDraw &pd : prepared)
    {
        if (pd.isResolve)
        {
            // A resolve that also clears -- or only clears. The clear happens at
            // THIS point in the stream, which is the whole point: on a tiled
            // frame the guest clears depth once per tile, and doing it only at
            // the start of the frame leaves the second tile testing against the
            // first tile's depth.
            auto doDepthClear = [&]()
            {
                if (!pd.clearsDepth)
                    return;
                // WHICH depth buffer. There is one per RB_DEPTH_INFO base now,
                // and a clear that lands on whichever was bound last would
                // clear the wrong one -- the shadow atlas's clear wiping the
                // scene's depth is the same class of bug as the stencil one
                // this split exists to fix (catalog #91).
                DepthTarget *cdt = nullptr;
                if (!RT.GetDepthTarget(pd.depthTargetBase, pd.sampleLayout, cdt) || !cdt)
                    return;
                cdt->usedThisFrame = true;
                VkImageSubresourceRange dr{kDepthAspects, 0, 1, 0, 1};
                VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = P.depth;
                b.subresourceRange = dr;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                     &b);
                VkClearDepthStencilValue cv{pd.depthClearValue, pd.stencilClearValue};
                vkCmdClearDepthStencilImage(cmd, P.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv,
                                            1, &dr);
                VkImageMemoryBarrier r{b};
                r.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                r.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                r.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                r.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0,
                                     nullptr, 1, &r);
                ++depthClearsDone;
            };
            // A DEPTH resolve: sample the host depth image and write the depth
            // the guest's k_24_8_FLOAT fetch would have produced.
            if (pd.resolveIsDepth)
            {
                endPass();
                // The copy reads the depth buffer of ITS OWN base, not of
                // whichever pass ran last: this frame resolves the scene's
                // depth (base 0x0) and the shadow atlas's (0x5a0), and reading
                // the wrong one copies out a different pass's picture.
                DepthTarget *rdt = nullptr;
                RT.GetDepthTarget(pd.depthTargetBase, pd.sampleLayout, rdt);
                auto dst = P.resolveTargets.find(pd.resolveDest);
                const VkPipeline depthResolvePipeline = rdt && rdt->samples == VK_SAMPLE_COUNT_2_BIT
                                                            ? P.resolveDepth2xPipeline
                                                            : P.resolveDepthPipeline;
                if (dst != P.resolveTargets.end() && depthResolvePipeline != VK_NULL_HANDLE &&
                    dst->second.storageView != VK_NULL_HANDLE &&
                    RT.resolveDepthSetsUsed < RT.resolveDepthSets.size())
                {
                    RT.ResolveDepthTo(cmd, dst->second, pd.resolveSrcRect, pd.resolveDstX,
                                      pd.resolveDstY, pd.resolveDepthIsFloat24,
                                      msaaModel ? draw::DeriveResolveSampling(
                                                      pd.surfaceInfo, pd.resolveSampleSelect, true)
                                                : draw::ResolveSampling{});
                    depthResolvesFloat24 += pd.resolveDepthIsFloat24 ? 1 : 0;
                    ++depthResolvesDone;
                    // A DEPTH COPY IS A PASS LIKE ANY OTHER, and leaving it out
                    // of the per-resolve snapshot is what made the pass-by-pass
                    // comparison report the depth copies as never executed
                    // (issue #90): they ran, nothing captured them, and a pass
                    // absent from the output is indistinguishable there from a
                    // pass the renderer skipped. ResolveDepthTo leaves the
                    // destination in SHADER_READ_ONLY_OPTIMAL, the same layout
                    // the colour path's snapshot expects.
                    if (dumpEachResolve &&
                        !snapshotResolveTarget(dst->second, resolveOrdinal, pd.diagIndex,
                                               pd.surfaceBase, pd.resolveDestFormat))
                        ++resolveSnapshotsFailed;
                    ++resolveOrdinal;
                }
                else
                {
                    ++depthResolvesSkipped;
                }
                doDepthClear();
                continue;
            }
            if (!pd.copyIsServed)
            {
                // Clear-only: no copy to perform, but the pass still has to end
                // so the depth image can be cleared outside it.
                endPass();
                doDepthClear();
                continue;
            }
            // The source surface must be outside a render pass to be blitted,
            // and it is left in TRANSFER_SRC_OPTIMAL by whichever pass wrote it.
            SurfaceTarget *src = nullptr;
            RT.GetSurfaceTarget(pd.surfaceBase, pd.sampleLayout, src);
            auto dst = P.resolveTargets.find(pd.resolveDest);
            if (!src || dst == P.resolveTargets.end() || !src->begunThisFrame)
                continue; // nothing has been rendered into it yet this frame
            endPass();
            // A RESOLVE IS A READ, and it reads EDRAM under RB_COLOR_INFO's
            // format at this point in the stream -- `pd.colorFormat` is that
            // register for resolve entries exactly as it is for geometry.
            //
            // This is where the frame's damage was escaping. The reinterpret
            // trigger further down never sees a resolve, because the resolve
            // branch `continue`s above it; the `isResolve` term added there was
            // dead. So a surface left in a float interpretation by a blending
            // draw was copied out lifted: catalog #83's wall pixel (640,350)
            // goes to (2.547, 4.031, 11.125) at draw 650 and the resolves at
            // 657 and 659 carried that away before anything restored it.
            if (reinterpretEnabled && src->storageFormat != UINT32_MAX)
            {
                const uint32_t want = draw::StorageColorFormat(pd.resolveSrcFormat);
                if (src->storageFormat != want)
                {
                    // A REFUSED CONVERSION MUST NOT RELABEL -- see the same
                    // rule at the geometry-draw site below. The bits were not
                    // touched, and a surface that claims a format its contents
                    // are not in makes the NEXT conversion convert from a
                    // format the data was never in.
                    if (RT.ReinterpretSurface(cmd, *src, src->storageFormat, want))
                        src->storageFormat = want;
                }
            }
            RT.ResolveSurfaceTo(cmd, *src, dst->second, pd.resolveSrcRect, pd.resolveDstX,
                                pd.resolveDstY, pd.resolveScale, pd.resolveSwapRB,
                                msaaModel ? draw::DeriveResolveSampling(
                                                pd.surfaceInfo, pd.resolveSampleSelect, false)
                                          : draw::ResolveSampling{});
            // Snapshot BEFORE the next resolve can touch this destination. The
            // ordinal advances whether or not the snapshot succeeds, so the
            // numbering keeps matching the oracle's even when one copy fails.
            if (dumpEachResolve && !snapshotResolveTarget(dst->second, resolveOrdinal, pd.diagIndex,
                                                          pd.surfaceBase, pd.resolveDestFormat))
                ++resolveSnapshotsFailed;
            ++resolveOrdinal;
            doDepthClear();
            ++resolvesDone;
            continue;
        }
        if (onlyDraw >= 0 && long(drawn) == onlyDraw)
            ++onlyDrawMatched;
        if (onlyDraw >= 0 && long(drawn) != onlyDraw)
        {
            ++drawn;
            continue;
        }
        if (PB.CheckpointDue(drawn))
        {
            // THE TARGET MUST BE READ BEFORE endPass(), which nulls it. Taking the
            // checkpoint afterwards passed a null every single time, so this knob
            // has never written an image or logged a line -- it looked exactly like
            // a frame with nothing to check.
            // The `last` fallback covers the draw right after a resolve, where no
            // pass is open yet: without it those checkpoints vanished with no line,
            // and they are the ones that say whether a pass's output survived to
            // its resolve.
            SurfaceTarget *const checkpointTarget = openTarget ? openTarget : lastTarget;
            const uint32_t checkpointBase = openTarget ? openSurface : lastSurface;
            endPass();
            PB.Checkpoint(cmd, drawn, checkpointTarget, checkpointBase);
        }
        // One texel, every draw. Same pass-boundary requirement as a checkpoint:
        // the copy cannot happen inside a render pass.
        if (PB.Tracing())
        {
            SurfaceTarget *t = openTarget ? openTarget : lastTarget;
            uint32_t base = openTarget ? openSurface : lastSurface;
            // With GEARS_DRAW_SURFACE set, sample THAT surface after every draw
            // rather than only when it is the bound one -- see PinnedSurface().
            if (PB.PinnedSurface() >= 0)
            {
                SurfaceTarget *pinned = nullptr;
                const uint32_t pinnedBase = uint32_t(PB.PinnedSurface());
                if (RT.GetSurfaceTarget(pinnedBase, pd.sampleLayout, pinned) && pinned)
                {
                    t = pinned;
                    base = pinnedBase;
                }
            }
            endPass();
            PB.TracePixel(cmd, drawn, lastIssuedPrep, t, base);
        }
        // The whole surface, in its own format, after a NAMED draw. Offered the
        // DIAG index of the draw that was last ISSUED -- the probe is sampling
        // what is on the surface now, which is that draw's output, and naming it
        // by the index every table in this project uses is the whole point of
        // the knob (see gpu_draw_probe.h).
        if (PB.Dumping() && lastIssuedPrep < prepared.size())
        {
            SurfaceTarget *t = openTarget ? openTarget : lastTarget;
            uint32_t base = openTarget ? openSurface : lastSurface;
            if (PB.PinnedSurface() >= 0)
            {
                SurfaceTarget *pinned = nullptr;
                const uint32_t pinnedBase = uint32_t(PB.PinnedSurface());
                if (RT.GetSurfaceTarget(pinnedBase, pd.sampleLayout, pinned) && pinned)
                {
                    t = pinned;
                    base = pinnedBase;
                }
            }
            endPass();
            PB.DumpSurface(cmd, drawn, lastIssuedPrep, prepared[lastIssuedPrep].diagIndex, t, base,
                           prepared[lastIssuedPrep].psHash, prepared[lastIssuedPrep].colorMask);
        }
        // The DEPTH buffer after a NAMED draw. There is one depth image, so no
        // surface is chosen and GEARS_DRAW_SURFACE does not apply -- what makes
        // this the right depth is that it is taken immediately after the draw.
        if (PB.DumpingDepth() && lastIssuedPrep < prepared.size())
        {
            endPass();
            PB.DumpDepth(cmd, drawn, lastIssuedPrep, prepared[lastIssuedPrep].diagIndex, P.depth,
                         depthFormat, prepared[lastIssuedPrep].psHash,
                         prepared[lastIssuedPrep].colorMask);
        }
        // The render comparer: a thumbnail of the surface after every draw.
        // Same pass-boundary requirement as the other two probes.
        if (PB.Comparing())
        {
            SurfaceTarget *t = openTarget ? openTarget : lastTarget;
            uint32_t base = openTarget ? openSurface : lastSurface;
            if (PB.PinnedSurface() >= 0)
            {
                SurfaceTarget *pinned = nullptr;
                const uint32_t pinnedBase = uint32_t(PB.PinnedSurface());
                if (RT.GetSurfaceTarget(pinnedBase, pd.sampleLayout, pinned) && pinned)
                {
                    t = pinned;
                    base = pinnedBase;
                }
            }
            endPass();
            PB.TraceAll(cmd, drawn, lastIssuedPrep, t, base);
        }
        // The EDRAM base this draw renders into may have been written under a
        // different colour format, and what is in the host image is VALUES in
        // that format rather than the console's bits. Convert before the draw
        // that reads them back -- with no pass open, which is why this sits
        // above the pass management below rather than inside beginPassOn().
        //
        // ONLY BEFORE A DRAW THAT ACTUALLY READS. The hardware does not convert
        // anything at a format change: the bits sit in EDRAM and whoever reads
        // them next interprets them under its own format. A draw that does not
        // read the destination -- no blending, so its own output simply
        // replaces what is there -- never observes the old bits, and converting
        // the surface for it rewrites every pixel it does not cover.
        //
        // That was measured, not reasoned: this condition used to fire on every
        // format change, and on walk_gameplay.gfr the draws that declare the
        // FLOAT layout cover 270,084 of 921,600 fragments while draw 649, which
        // declares the FIXED one, covers the screen and blends nothing. So 71%
        // of the frame was being lifted into an interpretation nothing ever
        // read it through, which is catalog #83's over-brightening (median
        // 0.141 against the oracle's 0.063) and why the pass had to ship off.
        //
        // APPROXIMATION, stated because it is one: `storageFormat` is per
        // SURFACE, so a PARTIAL non-blending write leaves the pixels it missed
        // labelled with its format. Exactness needs per-pixel provenance -- i.e.
        // holding EDRAM as bits, Xenia's FSI path -- which is a much larger
        // change. This frame's non-blending writes into 0x2d0 are full-screen
        // (649, 660, 716 at 921,600 fragments each), so each re-establishes a
        // uniform format and the approximation does not bite here. A frame
        // where it does will show up as a partial-coverage row in the count
        // below rather than silently.
        if (reinterpretEnabled)
        {
            SurfaceTarget *t = nullptr;
            const uint32_t want = draw::StorageColorFormat(pd.colorFormat);
            if (RT.GetSurfaceTarget(pd.surfaceBase, pd.sampleLayout, t) && t && t->begunThisFrame &&
                t->storageFormat != UINT32_MAX && t->storageFormat != want)
            {
                // A DRAW THAT WRITES NO COLOUR DECIDES NOTHING. It neither
                // reads the old bits nor replaces them, so it must not convert
                // AND must not relabel -- the surface still holds exactly what
                // it held, under the format it held it in. Relabelling on such
                // a draw is what broke the frame's shadow-mask pass (catalog
                // #91): draws 643/644/646 are depth/stencil-only (colour mask
                // 0) and carry RB_COLOR_INFO format k_8_8_8_8, so the 7e3 scene
                // in EDRAM 0x2d0 was silently relabelled k_8_8_8_8 without
                // being converted; draw 647, which DOES blend, then read a
                // destination our renderer believed was already 8888. The
                // console has no label to get wrong -- EDRAM holds bits, and
                // 647's blend there reads the 7e3 scene reinterpreted as 8888,
                // which is why its mask copy is a near-binary bright buffer
                // where ours carried the scene.
                // NOT `continue` -- a depth-only draw still has to be issued,
                // and skipping the rest of the loop body would drop its depth
                // and stencil writes. Only the reinterpretation is skipped.
                // Per-transition, on a debug channel: the frame line aggregates
                // by format PAIR, which cannot say WHICH draw met a change or
                // in what order -- the two questions needed to follow a
                // surface's interpretation through a pass (catalog #91).
                lucent::debug("draw",
                              "diag draw {} (ps {:#x}) meets format"
                              " change {} -> {} on surface {:#x}: mask {:#x} frag {}"
                              " blend {:#x}",
                              pd.diagIndex, pd.psHash, draw::ColorFormatName(t->storageFormat),
                              draw::ColorFormatName(want), pd.surfaceBase, pd.colorMask & 0xF,
                              pd.hasFragmentStage ? 1 : 0, pd.blend0);
                if ((pd.colorMask & 0xF) == 0 || !pd.hasFragmentStage)
                {
                    ++RT.reinterpretsNoWrite;
                }
                else
                {
                    // A resolve reads the surface by definition. A geometry draw
                    // reads it only when the blend equation is not the identity
                    // (src ONE, dst ZERO) -- the same predicate the pipeline uses
                    // to decide whether to enable blending at all.
                    const bool readsDestination = pd.isResolve || !draw::BlendIsIdentity(pd.blend0);
                    bool relabel = true;
                    if (readsDestination)
                    {
                        endPass();
                        // A REFUSED CONVERSION MUST NOT RELABEL. The bits were not
                        // touched, so a surface that now claims the new format is
                        // claiming a format its contents are not in -- and the next
                        // conversion then converts FROM a format the data was never
                        // in, which is not a missed improvement but active damage.
                        //
                        // Measured on walk_gameplay.gfr: draw 613 meets
                        // k_8_8_8_8 -> k_16_16, this pass refuses that pair, and the
                        // surface used to be relabelled k_16_16 anyway; draw 615
                        // then converted "k_16_16 -> k_2_10_10_10_FLOAT" on data
                        // that was still 8888. The two copies downstream of it
                        // (diag 639 and 657) came out at mean 0.0869 where the
                        // console has 0.1760, and at 0.1804 with the whole pass
                        // switched off -- so the conversion chain, not the idea of
                        // converting, was what moved them (catalog #95).
                        relabel = RT.ReinterpretSurface(cmd, *t, t->storageFormat, want);
                    }
                    else
                    {
                        ++RT.reinterpretsNotRead;
                        RT.reinterpretNotReadPairs.insert((uint64_t(t->storageFormat) << 32) |
                                                          want);
                    }
                    // A converted surface holds the new interpretation, and an
                    // unconverted one that is about to be overwritten by a draw
                    // writing in `want` will hold it shortly. A REFUSED one holds
                    // neither, and keeps the format its bits are in.
                    if (relabel)
                        t->storageFormat = want;
                }
            }
        }
        // Open a pass if there is none, or re-open on a different surface OR a
        // different depth base -- both are attachments of the framebuffer.
        const auto drawSamples = VkSampleCountFlagBits(pd.sampleLayout.rasterSamples);
        if (!inPass || openSurface != pd.surfaceBase || openDepthBase != pd.depthTargetBase ||
            openSamples != drawSamples)
        {
            if (inPass)
            {
                endPass();
                ++surfaceSwitches;
            }
            if (!beginPassOn(pd.surfaceBase, pd.depthTargetBase, pd.sampleLayout))
                continue;
        }
        if (openTarget && openTarget->storageFormat == UINT32_MAX)
            openTarget->storageFormat = draw::StorageColorFormat(pd.colorFormat);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pd.pipeline);
        vkCmdSetViewport(cmd, 0, 1, &pd.viewport);
        vkCmdSetScissor(cmd, 0, 1, &pd.scissor);
        vkCmdSetDepthBias(cmd, pd.depthBiasConstant, 0.0f, pd.depthBiasSlope);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pd.layout, 0, 4, pd.sets, 0,
                                nullptr);
        DS.Begin(cmd, drawn);
        if (pd.indexed)
        {
            vkCmdBindIndexBuffer(cmd, pd.ibuf, pd.ibufOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, pd.count, 1, 0, 0, 0);
        }
        else
        {
            vkCmdDraw(cmd, pd.count, 1, 0, 0);
        }
        DS.End(cmd, drawn);
        if (openTarget)
            ++openTarget->drawsThisFrame;
        // EDRAM COLOUR/DEPTH ALIASING, after the draw that causes it.
        //
        // The guest has pointed RB_DEPTH_INFO at the same EDRAM base this
        // draw's colour surface occupies, so on the console the depth and
        // stencil this draw just wrote landed IN that surface's colour bits,
        // and the next draw that blends against the surface reads them. Three
        // draws of the Act 1 frame do this, and the shadow-mask pass depends on
        // it: it blends src x dst where dst is the depth buffer this draw
        // filled (catalog #91).
        //
        // AFTER, not before. Running it before the draw copies the depth as it
        // stood BEFORE this draw wrote it, which is the scene's depth rather
        // than the value the mask pass reads -- measured, that moved the mask
        // copy by 0.02 where the aliasing is worth 0.25.
        if (reinterpretEnabled && !noAlias && pd.depthBase == pd.surfaceBase)
        {
            draw::SurfaceTarget *at = nullptr;
            if (RT.GetSurfaceTarget(pd.surfaceBase, pd.sampleLayout, at) && at &&
                at->begunThisFrame)
            {
                endPass();
                {
                    // Same rule as the depth resolve: alias the depth buffer
                    // this draw actually rendered against.
                    DepthTarget *adt = nullptr;
                    RT.GetDepthTarget(pd.depthTargetBase, pd.sampleLayout, adt);
                }
                RT.AliasDepthIntoSurface(cmd, *at, pd.resolveDepthIsFloat24);
            }
        }
        lastIssuedPrep = uint32_t(&pd - prepared.data());
        ++drawn;
    }
    // The LAST draw of the frame is never followed by another iteration, so
    // without this the one draw a dump is most likely to be aimed at -- the
    // final composite -- would report "never offered" and read as a missing
    // draw. DumpSurface ignores a diag index it has already taken.
    if (PB.Dumping() && lastIssuedPrep < prepared.size())
    {
        SurfaceTarget *t = openTarget ? openTarget : lastTarget;
        uint32_t base = openTarget ? openSurface : lastSurface;
        if (PB.PinnedSurface() >= 0)
        {
            SurfaceTarget *pinned = nullptr;
            const uint32_t pinnedBase = uint32_t(PB.PinnedSurface());
            if (RT.GetSurfaceTarget(pinnedBase, prepared[lastIssuedPrep].sampleLayout, pinned) &&
                pinned)
            {
                t = pinned;
                base = pinnedBase;
            }
        }
        endPass();
        PB.DumpSurface(cmd, drawn, lastIssuedPrep, prepared[lastIssuedPrep].diagIndex, t, base,
                       prepared[lastIssuedPrep].psHash, prepared[lastIssuedPrep].colorMask);
    }
    // The last draw's depth, for the same reason: aimed at the final draw, the
    // in-loop site never comes round again.
    if (PB.DumpingDepth() && lastIssuedPrep < prepared.size())
    {
        endPass();
        PB.DumpDepth(cmd, drawn, lastIssuedPrep, prepared[lastIssuedPrep].diagIndex, P.depth,
                     depthFormat, prepared[lastIssuedPrep].psHash,
                     prepared[lastIssuedPrep].colorMask);
    }
    endPass();

    // --- which surface is presented ---------------------------------------
    // The frame's final composite is whatever surface the LAST geometry draw
    // wrote: on this title's UE3 pipeline that is the 8888 tonemap/UI buffer at
    // EDRAM 0x2d0, reached by rule rather than by naming the address.
    uint32_t presentBase = 0;
    bool havePresent = false;

    // WHAT THE GUEST SAID ABOUT SCANOUT, DECODED. VdSwap posts the front
    // buffer's six-dword texture fetch constant (vd_null_gpu.cpp) and the
    // capture carries it, but nothing has ever read it: the rule below chooses
    // between the resolve SOURCE and its DESTINATION from a comment settled
    // against the boot movie, while this constant is the hardware's own
    // statement of how the front buffer is read back -- including the SWIZZLE,
    // which is exactly the red/blue question catalog #62 keeps returning to.
    //
    // A frame with no constant must say so rather than print a decode of zeros:
    // "swizzle XYZW" and "we never recorded a constant" would otherwise read
    // identically, and only one of them is evidence.
    {
        const uint32_t *fb = in.frontBufferFetch;
        const bool haveFetch = (fb[0] | fb[1] | fb[2] | fb[3] | fb[4] | fb[5]) != 0;
        // First frame, and every CHANGE after it. A per-frame line would bury a
        // change in six thousand identical ones, and a first-frame-only line
        // would report the boot movie's constant and never mention that
        // gameplay uses a different one -- which is the whole question here.
        static uint32_t reportedKey = 0;
        static bool reportedOnce = false;
        const uint32_t key = fb[0] ^ fb[1] ^ fb[2] ^ fb[3] ^ fb[4] ^ fb[5] ^ in.frontBufferAddress;
        const bool report = !reportedOnce || key != reportedKey;
        reportedOnce = true;
        reportedKey = key;
        if (!report)
        {
            // say nothing
        }
        else if (!haveFetch)
        {
            lucent::info("draw", "front-buffer fetch constant: NONE -- all six"
                                 " dwords are zero. This frame says NOTHING about the scanout"
                                 " format or swizzle; it does not say the swizzle is identity."
                                 " (A capture older than frame_capture version 3, or a swap"
                                 " packet written before VdSwap carried the constant.)");
        }
        else
        {
            static const char *kSwz[8] = {"X", "Y", "Z", "W", "0", "1", "?6", "?7"};
            const uint32_t swizzle = (fb[3] >> 1) & 0xFFFu;
            char swz[5] = {0};
            for (int i = 0; i < 4; ++i)
                swz[i] = kSwz[(swizzle >> (i * 3)) & 7][0];
            const uint32_t base = (fb[1] >> 12) << 12;
            const uint32_t fmt = fb[1] & 0x3Fu;
            const uint32_t endian = (fb[1] >> 6) & 3u;
            const uint32_t tiled = (fb[0] >> 31) & 1u;
            const uint32_t pitch = ((fb[0] >> 22) & 0x1FFu) * 32u;
            const uint32_t w = (fb[2] & 0x1FFFu) + 1u;
            const uint32_t h = ((fb[2] >> 13) & 0x1FFFu) + 1u;
            lucent::info("draw",
                         "front-buffer fetch constant: base {:#x} {}x{}"
                         " fmt {} endian {} tiled {} pitch {} SWIZZLE {} ({:#05x});"
                         " the guest's address for this frame is {:#x}",
                         base, w, h, fmt, endian, tiled, pitch, swz, swizzle,
                         in.frontBufferAddress);
            // The one line that matters for catalog #62. Say both readings out
            // loud, because "ZYXW" is only meaningful next to what it implies.
            if (swz[0] == 'Z' && swz[1] == 'Y' && swz[2] == 'X')
                lucent::info("draw", "  -> scanout reads the front buffer with"
                                     " RED AND BLUE EXCHANGED. A resolve that also swaps cancels"
                                     " against this, and the image a person sees is the resolve"
                                     " SOURCE, not the destination.");
            else if (swz[0] == 'X' && swz[1] == 'Y' && swz[2] == 'Z')
                lucent::info("draw", "  -> scanout reads the front buffer"
                                     " STRAIGHT. Nothing cancels a resolve swap, so the image a"
                                     " person sees is the resolve DESTINATION -- the front"
                                     " buffer's own bytes.");
        }
    }

    // FIRST, WHAT THE GUEST SAID. VdSwap carries the front buffer's address, and a
    // surface's resolve destination is where its pixels land in guest memory, so the
    // surface whose destination IS the front buffer is the one being shown. That is
    // a statement, not a rule of thumb.
    if (in.frontBufferAddress != 0)
    {
        const uint32_t front = in.frontBufferAddress & 0x1FFFFFFFu; // drop the alias
        for (const auto &pd : prepared)
        {
            if (!pd.isResolve || pd.resolveDest == 0)
                continue;
            if ((pd.resolveDest & 0x1FFFFFFFu) != front)
                continue;
            presentBase = pd.surfaceBase;
            havePresent = true;
            break;
        }
        if (!havePresent)
            lucent::debug("draw",
                          "front buffer {:#x} names no resolve destination in"
                          " this frame; falling back to the last-geometry-draw rule",
                          in.frontBufferAddress);
    }
    // The old rule is still computed, and DISAGREEMENT IS REPORTED. If it ever
    // differs from what the guest named, that frame is one the renderer would have
    // presented from the wrong surface -- the scene's linear-light HDR buffer rather
    // than the tonemapped one, which shows as flat unlit grey with every texture
    // detail intact. That is the shape of a reported symptom this project could not
    // reproduce, so the disagreement is worth a line rather than a silent
    // correction.
    uint32_t ruleBase = 0;
    for (auto it = prepared.rbegin(); it != prepared.rend(); ++it)
    {
        if (it->isResolve)
            continue;
        ruleBase = it->surfaceBase;
        break;
    }
    if (havePresent && ruleBase != 0 && ruleBase != presentBase)
        lucent::warn("draw",
                     "the guest's front buffer names surface {:#x} but the"
                     " last-geometry-draw rule would have picked {:#x} -- presenting the"
                     " guest's. Before this frame the rule decided, and on frames like it the"
                     " window showed the wrong buffer",
                     presentBase, ruleBase);
    for (auto it = prepared.rbegin(); !havePresent && it != prepared.rend(); ++it)
    {
        if (it->isResolve)
            continue;
        presentBase = it->surfaceBase;
        havePresent = true;
        break;
    }
    // The image the frame ends up in, left in TRANSFER_SRC_OPTIMAL. Published to the
    // presenter when the device is shared, so it can blit rather than receive the
    // pixels through host memory.
    VkImage presentableImage = VK_NULL_HANDLE;
    bool gpuScanoutGammaApplied = false;
    GpuScanoutResult scanoutResult;

    // WHETHER THE PIXELS ARE STILL NEEDED ON THE HOST. Since the presenter blits the
    // published image directly, the readback only serves this frame's own diagnostics
    // -- EXCEPT when this renderer owns its device, because then the presenter (if any)
    // cannot blit from an image on a different device and falls back to the host
    // upload, which reads exactly these pixels. Dropping the readback in that case
    // would present nothing at all, so the condition is deliberately generous.
    const bool needHostPixels = FrameNeedsHostPixels(in.report, in.probe) || ownsDevice;

    SurfaceTarget *presentTarget = nullptr;
    if (havePresent)
    {
        auto it = P.surfaceTargets.find(presentBase);
        if (it != P.surfaceTargets.end() && it->second.begunThisFrame)
            presentTarget = &it->second;
        // THE FAILURE WAS SILENT, AND IT IS THE ONE THAT MATTERS. When this
        // lookup misses, presentableImage stays null, no blit runs, and g_frame
        // KEEPS THE PREVIOUS FRAME'S PIXELS -- so the window and the screenshot
        // show a stale frame while every log line still describes the frame
        // that was just rendered. Catalog #86 spent six rounds of measurement
        // narrowing an inset image to this path and could not get inside it,
        // because the one branch that would explain it printed nothing. Which
        // of the two conditions failed is named, since 'no target for that
        // base' and 'a target that never began this frame' have different
        // causes.
        if (!presentTarget)
            lucent::warn("draw",
                         "PRESENTING NOTHING NEW: surface {:#x} {} --"
                         " the published image and the screenshot therefore still hold"
                         " the PREVIOUS frame's pixels, and every other line about this"
                         " frame describes pixels nobody saw",
                         presentBase,
                         it == P.surfaceTargets.end()
                             ? "has no render target at all"
                             : "has a render target that was never begun this frame");
    }

    if (presentTarget)
    {
        if (!P.scanout.Record(*this, cmd, presentTarget->color, W, H, in.gammaRamp, readback,
                              needHostPixels, scanoutResult))
            return false;
        presentableImage = scanoutResult.image;
        gpuScanoutGammaApplied = scanoutResult.gammaApplied;
    }
    // GEARS_DRAW_RESOLVE_DUMP=1: copy every resolve target out so it can be
    // written to a PPM after the frame. These images are what the guest's post
    // and tonemap passes SAMPLE, so when a frame looks wrong the question "is
    // the scene in the resolved texture, and does it look right there?" is the
    // one that splits a resolve defect from a shading defect -- and it cannot be
    // answered from the presented frame alone.
    // isDepth decides how the bytes are READ back. Getting this wrong does not
    // fail -- it silently reports 0.000 for every depth target, which is
    // indistinguishable from "the resolve wrote nothing".
    if (lucent::config::flag("DRAW_RESOLVE_DUMP"))
    {
        for (auto &[k, r] : P.resolveTargets)
        {
            if (!r.everWritten || r.width == 0 || r.imageHeight == 0)
                continue;
            const uint32_t bpp = r.isDepth ? 4u : 8u;
            const VkDeviceSize bytes = VkDeviceSize(r.width) * r.imageHeight * bpp;
            VkBuffer b = VK_NULL_HANDLE;
            VkDeviceMemory m = VK_NULL_HANDLE;
            if (!MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b, m, true))
                continue;
            VkImageMemoryBarrier tb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            tb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            tb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            tb.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            tb.srcQueueFamilyIndex = tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            tb.image = r.image;
            tb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &tb);
            VkBufferImageCopy rg{};
            rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rg.imageExtent = {r.width, r.imageHeight, 1};
            vkCmdCopyImageToBuffer(cmd, r.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &rg);
            VkImageMemoryBarrier rb2 = tb;
            rb2.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            rb2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            rb2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            rb2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &rb2);
            resolveDumps.push_back({r.base, r.width, r.imageHeight, r.sourceBase, r.pitch, r.height,
                                    r.guestFormat, r.isDepth, b, m, UINT32_MAX, 0});
        }
    }

    // GEARS_DRAW_SURFACE_RANGE=1: the RANGE OF A SURFACE, not of a resolve
    // destination.
    //
    // Every range this renderer reports comes from a resolve DESTINATION,
    // because those are the only images it dumps. That is the wrong side of the
    // question whenever a pass renders wrong: catalog #81 needed "does surface
    // 0x2d0 exceed 8.0" and there was no way to ask it, so the range had to be
    // inferred backwards through a resolve's exponent bias.
    //
    // Reports per channel, and counts the pixels ABOVE 1.0 -- an HDR surface
    // that never exceeds 1.0 is the specific thing worth noticing, and a max
    // alone can be a single stray pixel.
    struct SurfDump
    {
        uint32_t base;
        VkFormat fmt;
        VkBuffer buf;
        VkDeviceMemory mem;
    };
    std::vector<SurfDump> surfDumps;
    if (lucent::config::flag("DRAW_SURFACE_RANGE"))
    {
        for (auto &[base, st] : P.surfaceTargets)
        {
            uint32_t bpp = 0;
            if (st.hostFormat == VK_FORMAT_R8G8B8A8_UNORM)
                bpp = 4;
            else if (st.hostFormat == VK_FORMAT_R16G16B16A16_SFLOAT)
                bpp = 8;
            if (bpp == 0)
            {
                // REFUSED, not guessed. Reading these bytes as a format they are
                // not is how the resolve dump once reported 0.000 for every
                // depth target, which looks exactly like "nothing was written".
                lucent::warn("draw",
                             "GEARS_DRAW_SURFACE_RANGE: surface {:#x} is"
                             " host format {} which this probe cannot decode, so NO range"
                             " is reported for it",
                             base, uint32_t(st.hostFormat));
                continue;
            }
            const VkDeviceSize bytes = VkDeviceSize(W) * H * bpp;
            VkBuffer b = VK_NULL_HANDLE;
            VkDeviceMemory m = VK_NULL_HANDLE;
            if (!MakeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, b, m, true))
                continue;
            VkBufferImageCopy rg{};
            rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rg.imageExtent = {W, H, 1};
            vkCmdCopyImageToBuffer(cmd, st.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &rg);
            surfDumps.push_back({base, st.hostFormat, b, m});
        }
        if (surfDumps.empty())
            lucent::warn("draw", "GEARS_DRAW_SURFACE_RANGE: no surface could be"
                                 " read back, so this run says NOTHING about any surface's range");
    }

    VK_CHECK(vkEndCommandBuffer(cmd));

    AR.EndFrame();
    msDrawLoop = sinceStartMs() - msSetup;
    const auto tSubmit = Clock::now();
    VkFence &fence = P.fence;
    if (fence == VK_NULL_HANDLE)
    {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(device, &fi, nullptr, &fence));
    }
    else
    {
        VK_CHECK(vkResetFences(device, 1, &fence));
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(SharedGpuQueueAccess().Submit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    accumulate(msSubmit, tSubmit);

    for (const SurfDump &sd : surfDumps)
    {
        const uint32_t bpp = sd.fmt == VK_FORMAT_R8G8B8A8_UNORM ? 4u : 8u;
        const VkDeviceSize bytes = VkDeviceSize(W) * H * bpp;
        void *mapped = nullptr;
        if (vkMapMemory(device, sd.mem, 0, bytes, 0, &mapped) == VK_SUCCESS)
        {
            float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f};
            float hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
            double sum[4] = {0, 0, 0, 0};
            uint64_t above1 = 0;
            // WHERE the brightest pixel is, not just how bright. A range is
            // something to look at next, and looking means aiming
            // GEARS_DRAW_PIXEL_TRACE at a coordinate -- which was being found by
            // guesswork before this.
            size_t brightest = 0;
            float brightestVal = -1e30f;
            const size_t px = size_t(W) * H;
            for (size_t i = 0; i < px; ++i)
            {
                float v[4];
                if (bpp == 4)
                {
                    const uint8_t *p8 = static_cast<const uint8_t *>(mapped) + i * 4;
                    for (int c = 0; c < 4; ++c)
                        v[c] = p8[c] / 255.0f;
                }
                else
                {
                    const uint16_t *p16 = static_cast<const uint16_t *>(mapped) + i * 4;
                    for (int c = 0; c < 4; ++c)
                        v[c] = HalfToFloat(p16[c]);
                }
                bool over = false;
                for (int c = 0; c < 4; ++c)
                {
                    if (v[c] < lo[c])
                        lo[c] = v[c];
                    if (v[c] > hi[c])
                        hi[c] = v[c];
                    sum[c] += v[c];
                    if (c < 3 && v[c] > 1.0f)
                        over = true;
                }
                if (over)
                    ++above1;
                const float lum = std::max(std::max(v[0], v[1]), v[2]);
                if (lum > brightestVal)
                {
                    brightestVal = lum;
                    brightest = i;
                }
            }
            vkUnmapMemory(device, sd.mem);
            lucent::info("draw",
                         "surface {:#x} range: R {:.4f}..{:.4f} G"
                         " {:.4f}..{:.4f} B {:.4f}..{:.4f} | means R {:.4f} G {:.4f} B"
                         " {:.4f} | {} of {} px have a colour channel ABOVE 1.0 ({:.2f}%)",
                         sd.base, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], sum[0] / double(px),
                         sum[1] / double(px), sum[2] / double(px), above1, px,
                         100.0 * double(above1) / double(px));
            lucent::info("draw",
                         "  surface {:#x} brightest pixel is ({},{}) at"
                         " {:.4f} -- GEARS_DRAW_PIXEL_TRACE={},{} follows it",
                         sd.base, uint32_t(brightest % W), uint32_t(brightest / W), brightestVal,
                         uint32_t(brightest % W), uint32_t(brightest / W));
        }
        vkDestroyBuffer(device, sd.buf, nullptr);
        vkFreeMemory(device, sd.mem, nullptr);
    }

    for (const ResolveDump &rd : resolveDumps)
    {
        void *mapped = nullptr;
        const VkDeviceSize bytes = VkDeviceSize(rd.w) * rd.h * (rd.isDepth ? 4 : 8);
        if (vkMapMemory(device, rd.mem, 0, bytes, 0, &mapped) == VK_SUCCESS)
        {
            const std::string &dirStr = lucent::config::text("DRAW_DIR");
            const std::filesystem::path dumpDir = dirStr.empty()
                                                      ? std::filesystem::path("scratch/screenshots")
                                                      : std::filesystem::path(dirStr);
            const auto dumpStem = [&]()
            {
                return rd.ordinal == UINT32_MAX
                           ? std::format("resolve_{:08x}", rd.base)
                           : std::format("resolve_{:02}_src{}{:03X}_{}x{}_f{}_{:08x}_draw{}",
                                         rd.ordinal, rd.isDepth ? 'D' : 'C', rd.sourceBase,
                                         rd.destPitch, rd.destHeight, rd.guestFormat, rd.base,
                                         rd.drawIndex);
            };
            if (dumpResolveFloat && !rd.isDepth)
            {
                const std::filesystem::path raw =
                    dumpDir / (dumpStem() +
                               ResolveSampleExtentSuffix(rd.w, rd.h, rd.destPitch, rd.destHeight) +
                               ".rgba16f");
                std::ofstream out;
                if (EnsureParentDirectory(raw))
                    out.open(raw, std::ios::binary | std::ios::trunc);
                if (!out || !out.write(static_cast<const char *>(mapped), bytes))
                    lucent::warn("draw",
                                 "resolve target {:#x}: FAILED to write"
                                 " raw RGBA16F dump {}; the PPM remains available, but"
                                 " HDR values were NOT captured",
                                 rd.base, raw.string());
                else
                    lucent::info("draw",
                                 "resolve target {:#x}: raw RGBA16F"
                                 " dump {} ({} bytes, unclamped)",
                                 rd.base, raw.string(), bytes);
            }
            // R16G16B16A16_SFLOAT -> 8-bit, clamped. An HDR target holds values
            // well above 1, so the clamp is honest saturation, not a tonemap.
            std::vector<uint8_t> rgba(size_t(rd.w) * rd.h * 4);
            // A MAX ALONE CANNOT SAY "EMPTY", and reporting it as if it could is
            // how this dump lied. Not every resolve target is a colour image: the
            // motion-blur pass reads a two-channel SIGNED velocity buffer, whose
            // values are fractions of a pixel and frequently negative. Clamping
            // that to 8-bit writes a pure black PPM, and a max seeded at 0.0 that
            // only ever grows reports 0.000 -- so a working velocity buffer is
            // indistinguishable from one the renderer never wrote. Track the true
            // range, and count what is actually non-zero.
            double maxSeen = -std::numeric_limits<double>::infinity();
            double minSeen = std::numeric_limits<double>::infinity();
            uint64_t nonZero = 0, samples = 0;
            // PER CHANNEL, in float and before the clamp. Catalog #62 is a
            // frame-wide red deficit (R/G = 0.77) plus a ceiling, and the one
            // thing that would localise it is WHICH STAGE the ratio first
            // departs from the next one's -- a question the merged range above
            // cannot answer, because a max over all three channels is identical
            // whether red is short or not. The PPM cannot answer it either: it
            // clamps at 1.0 and an HDR scene target spends most of its range
            // above that.
            double chanSum[3] = {0, 0, 0};
            uint64_t chanCount[3] = {0, 0, 0};
            if (rd.isDepth)
            {
                // R32_SFLOAT depth, written as greyscale. Reverse-Z puts the
                // near plane at 1.0, so a correct depth buffer reads BRIGHT
                // where geometry is close.
                const float *src = static_cast<const float *>(mapped);
                for (size_t i = 0; i < size_t(rd.w) * rd.h; ++i)
                {
                    const float v = src[i];
                    maxSeen = std::max(maxSeen, double(v));
                    minSeen = std::min(minSeen, double(v));
                    ++samples;
                    if (v != 0.0f)
                        ++nonZero;
                    const uint8_t g = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                    rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g;
                    rgba[i * 4 + 3] = 255;
                }
            }
            else
            {
                const uint16_t *src = static_cast<const uint16_t *>(mapped);
                for (size_t i = 0; i < size_t(rd.w) * rd.h; ++i)
                    for (int c = 0; c < 4; ++c)
                    {
                        const float v = HalfToFloat(src[i * 4 + c]);
                        if (c < 3)
                        {
                            maxSeen = std::max(maxSeen, double(v));
                            minSeen = std::min(minSeen, double(v));
                            ++samples;
                            if (v != 0.0f)
                                ++nonZero;
                            // NaN would silently poison a sum and print as
                            // -nan, which says nothing about how many samples
                            // were bad. Counted per channel instead.
                            if (v == v)
                            {
                                chanSum[c] += double(v);
                                ++chanCount[c];
                            }
                        }
                        rgba[i * 4 + c] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
            }
            const std::filesystem::path out =
                dumpDir /
                // A per-resolve snapshot is named by its ORDINAL FIRST, so the
                // files sort in the order the oracle's IssueCopy log lists them
                // and resolve N pairs with resolve N by filename alone.
                // Ordinal first so the files sort in execution order, then the
                // structural key the cross-emulator join needs (see ResolveDump)
                // and finally this run's own destination address and draw index,
                // which are ours alone and are for reading the log, not for
                // pairing.
                (dumpStem() + ".ppm");
            if (WritePpm(out, rgba.data(), rd.w, rd.h))
            {
                const double lo = samples ? minSeen : 0.0;
                const double hi = samples ? maxSeen : 0.0;
                lucent::info("draw",
                             "resolve target {:#x} ({}x{}) dumped to {}"
                             " (range {:.6f} .. {:.6f}, {} of {} components non-zero"
                             " [{:.1f}%]){}",
                             rd.base, rd.w, rd.h, out.string(), lo, hi, nonZero, samples,
                             samples ? 100.0 * double(nonZero) / double(samples) : 0.0,
                             nonZero != 0 && hi <= 1.0 / 255.0 && lo >= -1.0 / 255.0
                                 ? " -- NOT EMPTY: every value is below one 8-bit step, so"
                                   " the PPM is black and tells you nothing. This is what a"
                                   " velocity or similar sub-unit buffer looks like"
                                 : "");
                // The per-channel means, and the ratio catalog #62 is about.
                // A depth target has no channels to compare, and a target whose
                // green mean is zero has no ratio at all -- both say so rather
                // than printing a number that reads like a measurement.
                if (!rd.isDepth)
                {
                    const double mr = chanCount[0] ? chanSum[0] / double(chanCount[0]) : 0.0;
                    const double mg = chanCount[1] ? chanSum[1] / double(chanCount[1]) : 0.0;
                    const double mb = chanCount[2] ? chanSum[2] / double(chanCount[2]) : 0.0;
                    lucent::Line cl;
                    cl.add("  channel means R {:.6f} G {:.6f} B {:.6f}", mr, mg, mb);
                    if (mg > 0.0)
                        cl.add("; R/G {:.4f} B/G {:.4f}", mr / mg, mb / mg);
                    else
                        cl.add("; NO RATIO: green sums to zero here, so this target"
                               " cannot say anything about a per-channel deficit");
                    // Samples dropped for being NaN are a defect in the target,
                    // not a rounding detail -- an unreported NaN count is how a
                    // mean quietly becomes a mean of the survivors.
                    const uint64_t perChan = samples / 3;
                    for (int c = 0; c < 3; ++c)
                        if (chanCount[c] != perChan)
                            cl.add("; channel {} had {} NaN samples of {}", c,
                                   perChan - chanCount[c], perChan);
                    cl.flush(lucent::Level::Info, "draw");
                }
            }
            vkUnmapMemory(device, rd.mem);
        }
        vkDestroyBuffer(device, rd.buf, nullptr);
        vkFreeMemory(device, rd.mem, nullptr);
    }
    // ALWAYS, when the knob is on, including when nothing was captured. Silence
    // here would mean "the frame performs no resolves" and "every snapshot
    // failed" print the same way -- and the second is an instrument failure
    // being read as a fact about the frame.
    if (dumpEachResolve)
    {
        const size_t taken =
            std::count_if(resolveDumps.begin(), resolveDumps.end(),
                          [](const ResolveDump &d) { return d.ordinal != UINT32_MAX; });
        lucent::Line rl;
        rl.add("per-resolve snapshots: {} captured, of {} resolves this renderer"
               " EXECUTED and {} copy draws the frame CONTAINS",
               taken, resolveOrdinal, resolves.size());
        if (resolveSnapshotsFailed)
            rl.add("; {} FAILED to allocate a readback buffer and are MISSING"
                   " from the output -- a gap is that, not a resolve that wrote"
                   " nothing",
                   resolveSnapshotsFailed);
        if (resolveSnapshotsUnwritten)
            rl.add("; {} destination(s) were never written by their dispatch"
                   " (degenerate rectangle or an offset past the image) and are"
                   " MISSING from the output for that reason",
                   resolveSnapshotsUnwritten);
        // THE ORDINAL IS OURS, NOT THE ORACLE'S, and saying otherwise would be
        // worse than saying nothing. This renderer skips copy draws the oracle
        // performs -- depth resolves go down another path, and the untile
        // collapse drops a tile's worth -- so on bright.gfr we execute 14 where
        // Xenia executes 18 and ordinal 3 is a DIFFERENT resolve on each side.
        // PAIR BY THE DRAW INDEX in the filename: the oracle's Nth IssueCopy is
        // the Nth copy draw of the capture, which `gfr_to_xtr.py` lists with
        // its draw index.
        if (resolves.size() != size_t(resolveOrdinal))
            rl.add(". These ordinals are THIS RENDERER'S execution order and do"
                   " NOT match the oracle's, because {} copy draw(s) were not"
                   " executed here. Pair by the draw index in the filename",
                   resolves.size() - size_t(resolveOrdinal));
        else
            rl.add(". Every copy draw was executed, so these ordinals match the"
                   " oracle's IssueCopy order; pair by the draw index anyway");
        rl.flush(lucent::Level::Info, "draw");
        if (resolveOrdinal == 0)
            lucent::warn("draw", "GEARS_DRAW_RESOLVE_DUMP_EACH is on but this"
                                 " frame executed NO resolves, so nothing was captured and this"
                                 " run says nothing about any resolve");
    }

    // GEARS_DRAW_STREAM=<path>: ONE LINE PER FRAME describing WHAT THE GUEST
    // ASKED THE GPU TO DO -- the multiset of (vertex shader, pixel shader) pairs
    // it bound, with counts.
    //
    // This is the cross-emulator comparison that does not need determinism.
    // Catalog #84 establishes that no clock anchor makes two runs reach the same
    // state at the same frame while guest threads are host-scheduled, so a
    // per-pixel frame-indexed diff against the oracle is not available. But the
    // DRAW STREAM is the guest's own output -- it is what our CPU emulation
    // produces and hands to the GPU -- so if our emulation differs from the
    // console's, the work requested differs, and that shows up as a shader the
    // other side never binds or a count that does not match. Aligning two runs
    // by SIGNATURE SIMILARITY rather than by frame index tolerates the two sides
    // being at different moments, which they always are.
    //
    // The Xenia fork emits the identical format at its own swap, and
    // tools/draw_stream_compare.py reads both.
    {
        static const std::string &streamPath = lucent::config::text("DRAW_STREAM");
        if (!streamPath.empty())
        {
            static std::FILE *sf = std::fopen(streamPath.c_str(), "wb");
            static uint64_t streamFrame = 0;
            const uint64_t thisFrame = streamFrame++;
            if (sf)
            {
                std::map<std::pair<uint64_t, uint64_t>, uint32_t> counts;
                for (const draw::PreparedDraw &pd : prepared)
                {
                    // NORMALISED TO WHAT THE OTHER SIDE MEANS BY IT. Xenia sets
                    // pixel_shader = nullptr on a draw with no fragment stage,
                    // so its stream records ps = 0 there. We record whatever the
                    // guest last programmed, which is a real pixel shader hash --
                    // and the two conventions made every depth-only draw look
                    // like a shader pair one side "never binds": 8 of the 13
                    // pairs in the first comparison's ONLY-OURS list were this,
                    // 680,525 draws at the top of it. A recording convention
                    // reading as the headline divergence.
                    ++counts[{pd.vsHash, pd.hasFragmentStage ? pd.psHash : 0}];
                }
                std::string line = std::format("{}\t{}", thisFrame, prepared.size());
                for (const auto &[pair, n] : counts)
                    line += std::format("\t{:016x}:{:016x}:{}", pair.first, pair.second, n);
                line += '\n';
                std::fwrite(line.data(), 1, line.size(), sf);
                std::fflush(sf);
            }
        }
    }

    // GEARS_DRAW_STREAM_RAW=<path>: the same one-line-per-frame format, but
    // counted from what the GUEST PROGRAMMED rather than from what survived
    // preparation. Comparing this against the oracle answers a question the
    // prepared-level stream cannot: a shader missing from our stream is either
    // one the guest never binds (a CPU-emulation divergence) or one we bind and
    // drop (a renderer bug), and only a pre-drop recording separates them.
    if (wantRawStream)
    {
        static std::FILE *rf = std::fopen(rawStreamPath.c_str(), "wb");
        static uint64_t rawFrame = 0;
        const uint64_t thisFrame = rawFrame++;
        if (rf)
        {
            uint32_t rawTotal = 0;
            for (const auto &[pair, n] : rawCounts)
                rawTotal += n;
            std::string line = std::format("{}\t{}", thisFrame, rawTotal);
            for (const auto &[pair, n] : rawCounts)
                line += std::format("\t{:016x}:{:016x}:{}", pair.first, pair.second, n);
            line += '\n';
            std::fwrite(line.data(), 1, line.size(), rf);
            std::fflush(rf);
        }
        else
        {
            // A path that would not open must not read as "the guest programmed
            // nothing" -- the whole point of this arm is to distinguish an empty
            // answer from an absent one.
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                lucent::error("draw",
                              "GEARS_DRAW_STREAM_RAW={} could not be"
                              " opened for writing; NOTHING was recorded and this run says"
                              " nothing about what the guest programmed",
                              rawStreamPath);
            }
        }
    }

    DS.Report(drawn, prepared);

    if (!ownsDevice && presentableImage != VK_NULL_HANDLE &&
        !P.scanout.Publish(scanoutResult, W, H, in.sequence))
        return false;

    // --- read pixels + coverage numbers ----------------------------------
    // Shared-device presentation never reads these bytes, so copying a full frame to host
    // every frame would be pure cost. g_frame keeps previous contents, which nothing reads;
    // if that changes, this is where they go stale.
    if (needHostPixels)
    {
        // The readback allocation may include MSAA rows; the host contract is W x H RGBA.
        g_frame.resize(size_t(W) * H * 4);
        std::memcpy(g_frame.data(), P.readbackMapped, g_frame.size());
        // Scan-out gamma. The console puts every presented pixel through the
        // guest's DC_LUT ramp; without it the image is brighter than the
        // console's, and this title's ramp is markedly non-linear (measured: 254
        // of 256 entries differ from linear, darkening the low end -- input 4 of
        // 255 maps to 6 of 1023 where linear would give 16).
        //
        // On a shared device the published image has already passed through the
        // GPU LUT, and this readback is of those transformed pixels. Headless and
        // cross-device runs instead use this authoritative CPU implementation.
        if (in.gammaRamp && !gpuScanoutGammaApplied)
        {
            const ScanoutGammaLut scanoutGammaLut = BuildScanoutGammaLut(in.gammaRamp);
            if (!ApplyScanoutGamma(g_frame, scanoutGammaLut))
                return false;
        }
    }
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
            const uint8_t *px = &g_frame[size_t(i) * 4];
            if (!(px[0] == 13 && px[1] == 13 && px[2] == 20)) // != the clear
                ++changed;
            if (px[0] || px[1] || px[2])
                ++lit;
        }
        std::set<std::pair<uint64_t, uint64_t>> pairs;
        for (const FrameDrawItem &d : in.draws)
            pairs.emplace(d.vsHash, d.psHash);
        lucent::info("draw",
                     "frame: {} of {} draws issued, {} skipped; {} distinct shader"
                     " pairs, {} distinct shaders, {} pipelines, {} texture layouts,"
                     " {} pipeline layouts; {} texture bindings ({} guest textures,"
                     " {} from the rendered RT, {} from a stub); {}/{} px non-black"
                     " ({:.1f}%), {} px changed from the clear ({:.1f}%)",
                     issued, in.draws.size(), CN.skipped, pairs.size(), SC.modules.size(),
                     PC.pipelines.size(), PC.texLayouts.size(), PC.pipeLayouts.size(), TB.Binds(),
                     TB.bindsGuest, TB.bindsRt, TB.bindsStub, lit, uint64_t(W) * H,
                     100.0 * double(lit) / (double(W) * H), changed,
                     100.0 * double(changed) / (double(W) * H));

        // WHERE THE UNIFORM TIME GOES. Uniforms are the largest item in the frame
        // (118 ms of 187), and this cache exists to avoid recomputing blocks that
        // did not change. Reported with its denominator so a low hit rate is
        // distinguishable from a cache that is never consulted, and split by WHICH
        // part of the key differed -- the register snapshot is compared by POINTER,
        // so a draw carrying its own copy would miss however identical the contents,
        // and that failure would look exactly like "the constants really do change".
        lucent::info("draw",
                     "uniform cache: {} lookup(s), {} hit ({:.1f}%);"
                     " misses: {} on the register snapshot, {} on the shader pair",
                     UC.lookups, UC.hits,
                     UC.lookups ? 100.0 * double(UC.hits) / double(UC.lookups) : 0.0,
                     UC.missSnapshot, UC.missShaders);
        // WHAT IS IN the blocks, not just how often they were rebuilt. A NaN
        // here takes a whole frame to black and no draw-level probe can see it.
        UC.ReportConstantCensus();
        if (UC.recomputes != 0)
            lucent::info("draw",
                         "uniform headroom: of {} recompute(s) measured,"
                         " {} produced BYTE-IDENTICAL blocks ({:.1f}%) -- that is the work a"
                         " content comparison would skip",
                         UC.recomputes, UC.recomputesIdentical,
                         100.0 * double(UC.recomputesIdentical) / double(UC.recomputes));
        else
            lucent::info("draw", "uniform headroom: not measured (set"
                                 " GEARS_DRAW_UBOCHECK=1); a percentage cannot be inferred from the"
                                 " hit rate alone");

        // WHAT THE TEXTURE CACHE EVICTED. The cache is keyed on the fetch
        // constant, which does not change when the guest overwrites the pixels at
        // the same address, so the guest bytes are re-hashed once per frame per
        // distinct texture and a changed one is evicted and re-uploaded. Reported
        // with its denominator, because "0 evicted" only means something next to
        // the number of textures that were actually checked -- a zero denominator
        // would mean the check never ran, which is a different statement.
        gears::native::ReportRoster();
        lucent::info("draw",
                     "descriptor sets: {} draws reused an earlier draw's"
                     " texture sets, {} built new ones, {} distinct sets in the frame."
                     " The key is the shader pair plus the fetch constants behind every"
                     " texture and sampler binding, so a reuse skips resolving those"
                     " bindings entirely; the uniform sets are always the draw's own",
                     DB.hits, DB.builds, DB.CacheSize());
        IP.Report();
        TX.Report();

        if (PC.rectDraws)
            lucent::info("draw",
                         "frame rectangle lists: {} of {} draws expanded by a"
                         " geometry shader ({} distinct)",
                         PC.rectDrawsExpanded, PC.rectDraws, PC.geomShaders.size());
        {
            CN.ReportSurfaces();
        }
        {
            CN.ReportModes();
        }
        {
            lucent::Line rl;
            rl.add("render-target cache: {} host surfaces (surface:format ->"
                   " host format, draws this frame):",
                   P.surfaceTargets.size());
            for (const auto &[base, t] : P.surfaceTargets)
                rl.add(" {:#x}->vk{}x{}", base, uint32_t(t.hostFormat), t.drawsThisFrame);
            rl.flush(lucent::Level::Info, "draw");
            lucent::Line dl;
            dl.add("render-target cache: {} resolve destinations (dest<-copies):",
                   P.resolveTargets.size());
            for (const auto &[base, r] : P.resolveTargets)
                dl.add(" {:#x}<-{}", base, r.copies);
            dl.flush(lucent::Level::Info, "draw");
            // Says WHICH RULE picked the surface. The old wording claimed the
            // guest's front-buffer address chose it even on frames where that
            // address was zero and the fallback decided -- which is every replay of
            // a v1 capture.
            lucent::info("draw",
                         "render-target cache: presenting surface {:#x},"
                         " chosen by {} (front buffer {:#x}); the guest's copy swap is NOT"
                         " undone here and must not be -- the scanout format reads it back"
                         " swapped, see the Epic-logo evidence in catalog #64;"
                         " resolves issued {}, skipped {}; {} distinct"
                         " RB_DEPTH_INFO depth bases (one shared host depth image)",
                         presentBase,
                         in.frontBufferAddress != 0 ? "the guest's front-buffer address"
                                                    : "the LAST-GEOMETRY-DRAW FALLBACK (no"
                                                      " front-buffer address in this frame)",
                         in.frontBufferAddress, CN.issuedResolves, CN.skippedResolves,
                         CN.depthBases.size());
            {
                CN.ReportDepthPairs();
            }
        }
        {
            // One line per distinct (source surface -> destination) pair; a
            // frame resolves the same surface once per predicated tile, so the
            // raw event list is long and the pairing is what matters.
            std::map<std::string, uint32_t> pairs;
            for (const ResolveEvent &re : resolves)
                ++pairs[re.srcIsDepth ? std::format("depth@{:#x} -> {:#x}", re.srcBase, re.destBase)
                                      : std::format("color{}@{:#x}:f{} -> {:#x}", re.srcSelect,
                                                    re.srcBase, re.srcFormat, re.destBase)];
            lucent::info("draw",
                         "frame resolves: {} copy draws, {} distinct"
                         " source->destination pairs",
                         resolves.size(), pairs.size());
            // The guest's own clear values, which the render target cache does
            // NOT yet use -- our depth clear is a host constant (catalog #31).
            // Reported so the values are visible before they are trusted.
            std::map<std::string, uint32_t> clears;
            for (const ResolveEvent &re : resolves)
            {
                if (!re.colorClear && !re.depthClear)
                    continue;
                ++clears[std::format("cmd{} color{}={:#010x} depth{}={:#010x}"
                                     " (depth base {:#x} fmt {})",
                                     re.copyCommand, re.colorClear ? "" : "(off)",
                                     re.colorClearValue, re.depthClear ? "" : "(off)",
                                     re.depthClearValue, re.depthBase, re.depthFormat)];
            }
            // Every resolve's rectangle and destination, which is how the
            // frame's predicated tiles are assembled -- or, while the resolve
            // blit ignores the rectangle, are not (catalog #32).
            for (const ResolveEvent &re : resolves)
                // WHICH copy carries a clear, not just how many the frame
                // programs. A mid-frame clear is a pass boundary: the aggregate
                // count says the frame has two, and cannot say that the colour
                // one lands right before the mask pass (catalog #91).
                lucent::info("draw",
                             "  resolve draw {}: {}@{:#x} -> {:#x}"
                             " rect [{} {}] [{} {}] [{} {}] window ({},{}) clears:{}{}{}",
                             re.drawIndex, re.srcIsDepth ? "depth" : "color", re.srcBase,
                             re.destBase, re.rect[0], re.rect[1], re.rect[2], re.rect[3],
                             re.rect[4], re.rect[5], re.windowX, re.windowY,
                             re.colorClear ? " COLOUR" : "", re.depthClear ? " DEPTH" : "",
                             (re.colorClear || re.depthClear) ? "" : " none",
                             re.haveRect ? "" : " (NO RECT: vf0 is not 3x2 floats)");
            for (const ResolveEvent &re : resolves)
                lucent::info("draw",
                             "  resolve draw {} destination: {:#x}"
                             " pitch {} height {} format {} number {} exp_bias {}"
                             " endian {} swap {}",
                             re.drawIndex, re.destBase, re.destPitch, re.destHeight, re.destFormat,
                             re.destNumber, re.destExpBias, re.destEndian, re.destSwap);
            lucent::Line cl;
            cl.add("frame clears programmed by resolves: {} of {} copy draws"
                   " carry a clear;",
                   clears.size(), resolves.size());
            for (const auto &[k, n] : clears)
                cl.add(" [{}x {}]", n, k);
            cl.flush(lucent::Level::Info, "draw");
            for (const auto &[what, n] : pairs)
                lucent::info("draw", "  resolve {} x{}", what, n);
        }
        // WARN when any draw fetches past the mirror, not info. Those draws read
        // zero and every primitive collapses at clipping, so the frame on screen
        // is missing world geometry -- and it still looks like a frame, which is
        // how a 64 MiB mirror once passed for a rendering bug and how a stale
        // capture later passed for a broken renderer (catalog #30, #57).
        {
            CN.ReportReach(in.guestPhysicalMirrorBytes);
        }
        // What the draw-index knobs actually selected. A knob that matched
        // nothing has to say so: silence is what a draw with nothing to show
        // produces too.
        draw::ReportDrawSelections();
        // A NEGATIVE THAT CARRIES ITS DENOMINATOR: "no signed fetches" has to be
        // distinguishable from "nobody looked".
        if (TB.fetchesWithSigns.empty())
            lucent::info("draw",
                         "frame texture signs: 0 of {} bindings ask for a"
                         " signed or gamma component, so the decode is a no-op on this frame"
                         " -- printed with its denominator so it is not mistaken for nobody"
                         " having looked",
                         TB.Binds());
        else
        {
            lucent::Line sl;
            sl.add("frame texture signs: {} distinct sign patterns among {} bindings"
                   " (0x3f is kGamma on RGB, 0x55 is kSigned on all four). These are"
                   " decoded per the fetch constant; GEARS_DRAW_NO_TEX_SIGNS=1 is the"
                   " control arm that reads them all unsigned:",
                   TB.fetchesWithSigns.size(), TB.Binds());
            for (const auto &[bits, n] : TB.fetchesWithSigns)
                sl.add(" [{:#04x} x{}]", bits, n);
            sl.flush(lucent::Level::Info, "draw");
        }
        if (!TB.signedBases.empty())
        {
            lucent::Line bl;
            bl.add("frame kSigned guest textures: {} distinct bases need a"
                   " signed-normalized host view, which this renderer does not create --"
                   " the unsigned view is bound to both slots, so these fixed-point"
                   " fetches read 0..1 where the shader expects -1..1:",
                   TB.signedBases.size());
            for (const auto &[b, n] : TB.signedBases)
                bl.add(" [{:#x} x{}]", b, n);
            bl.flush(lucent::Level::Warn, "draw");
        }
        lucent::info("draw",
                     "frame textures: {} distinct fetch constants, {} uploaded"
                     " ({:.1f} MiB), {} samplers",
                     TX.texDistinct.size(), TX.uploads.size(),
                     double(TX.uploadedBytes) / (1024.0 * 1024.0), TX.samplerCache.size());
        for (const auto &[fmt, n] : TX.texFormatBindings)
            lucent::info("draw", "  format {}: {} distinct fetches", fmt, n);
        for (const auto &[why, n] : TX.texSkips)
            lucent::warn("draw", "  NOT uploaded, {} distinct fetches: {}", n, why);
        for (const auto &[what, n] : TX.texFormatCensus)
            lucent::info("draw", "  texture {} x{}", what, n);
        CN.ReportViewports();
        {
            std::map<uint32_t, uint32_t> prims;
            for (const FrameDrawItem &d : in.draws)
                ++prims[d.primType];
            lucent::Line pl;
            pl.add("frame primitive types:");
            for (const auto &[p, n] : prims)
                pl.add(" {}x{}", PrimName(p), n);
            pl.flush(lucent::Level::Info, "draw");
        }
        CN.ReportSkips(in.draws.size());

        {
            lucent::Line tb;
            if (!TB.depthDestSamplers.empty())
            {
                lucent::Line dl;
                dl.add("frame shaders sampling a DEPTH resolve destination"
                       " (served by the depth resolve):");
                for (const auto &[k, n] : TB.depthDestSamplers)
                    dl.add(" {:#x}<-ps{:016x}x{}", k.first, k.second, n);
                dl.flush(lucent::Level::Info, "draw");
            }
            tb.add("frame texture bases ({} distinct):", TB.baseCount.size());
            for (const auto &[base, n] : TB.baseCount)
            {
                // Three cases, and only the third is a routing miss: the base IS
                // a resolve target; the base falls INSIDE one (so the guest is
                // sampling a sub-rectangle of something we resolved, and we hand
                // it stale guest memory); or it is an ordinary guest asset.
                const char *tag = "";
                if (TB.baseRtCount.count(base))
                    tag = "(RT)";
                else
                    for (const auto &[k, r] : P.resolveTargets)
                    {
                        if (r.bpp == 0 || r.pitch == 0)
                            continue;
                        const uint64_t extent = uint64_t(r.pitch) * r.height * r.bpp;
                        if (base > r.base && base < r.base + extent)
                        {
                            tag = "(INSIDE-RT)";
                            break;
                        }
                    }
                tb.add(" {:#x}x{}{}", base, n, tag);
            }
            tb.flush(lucent::Level::Info, "draw");
        }
        if (RT.resolvesOutOfSets)
            lucent::warn("draw",
                         "frame resolves: {} had no descriptor set and were"
                         " DROPPED -- the pool was sized too small",
                         RT.resolvesOutOfSets);
        if (RT.resolvesUnstorable)
            lucent::warn("draw",
                         "frame resolves: {} could not run the compute"
                         " resolve (host cannot use the format as a storage image), so the"
                         " guest's exponent bias and red/blue swap were NOT applied",
                         RT.resolvesUnstorable);
        if (RT.resolveNoRect || RT.resolveNoFormat)
            lucent::warn("draw",
                         "frame resolves: {} without a readable vf0"
                         " rectangle (whole surface copied), {} with a destination format"
                         " of unknown size (cannot be placed in a texture)",
                         RT.resolveNoRect, RT.resolveNoFormat);
        lucent::info("draw",
                     "frame depth resolves: {} executed ({} encoded as"
                     " float24/kD24FS8, {} as unorm24/kD24S8, from RB_DEPTH_INFO at each"
                     " copy), {} skipped",
                     depthResolvesDone, depthResolvesFloat24,
                     depthResolvesDone - depthResolvesFloat24, depthResolvesSkipped);
        {
            lucent::Line al;
            al.add("frame EDRAM colour/depth aliasing: {} draw(s) served", RT.depthAliasesDone);
            if (RT.depthAliasOutOfSets)
                al.add(", {} left unserved for want of a descriptor set", RT.depthAliasOutOfSets);
            if (RT.depthAliasRefused)
            {
                al.add(", {} REFUSED because the surface's storage format is one"
                       " this pass does not unpack:",
                       RT.depthAliasRefused);
                for (uint32_t f : RT.depthAliasRefusedFormats)
                    al.add(" {}", draw::ColorFormatName(f));
            }
            if (noAlias)
                al.add(" -- GEARS_DRAW_NOALIAS=1, so the two memories were kept"
                       " separate and no draw was served");
            al.flush(lucent::Level::Info, "draw");
        }
        lucent::info("draw",
                     "frame stencil: {} of {} pipelines built this frame"
                     " have RB_DEPTHCONTROL.stencil_enable set",
                     PC.pipelinesWithStencil, PC.pipelinesBuilt);
        lucent::info("draw",
                     "frame mid-stream depth clears: {} executed of {}"
                     " carried by the frame's copy draws (the guest clears depth once per"
                     " predicated tile)",
                     depthClearsDone, RT.midFrameDepthClears);
        if (onlyDraw >= 0)
            lucent::info("draw",
                         "GEARS_DRAW_ONLY={}: matched {} draw(s) of the"
                         " {} this frame ISSUED (the index counts issued draws, not the"
                         " diag table's guest index). It renders that draw over the"
                         " CLEAR, so a draw that samples a resolve target or a rendered"
                         " texture has no inputs here and produces black however correct"
                         " it is",
                         onlyDraw, onlyDrawMatched, drawn);
        lucent::info("draw",
                     "frame render pass: {} segments across {} surface"
                     " switches, {} resolves executed (RT link {})",
                     segments, surfaceSwitches, resolvesDone, TB.rtLinkEnabled ? "on" : "off");
        RT.ReportReinterpretation(reinterpretEnabled);

        lucent::info("draw", "scan-out gamma: {}",
                     in.gammaRamp
                         ? "the guest's ramp WAS applied to the published image and these"
                           " host pixels; the shared-device window, screenshots, census and"
                           " fallback upload now consume the same transformed scan-out"
                         : "no ramp programmed by the guest yet, so none applied -- this image"
                           " is the composite's own output");

        const std::string &dirStr = lucent::config::text("DRAW_DIR");
        const char *dir = dirStr.empty() ? nullptr : dirStr.c_str();
        const std::filesystem::path reportDir =
            dir ? std::filesystem::path(dir) : std::filesystem::path("scratch/screenshots");
        std::filesystem::path out = in.sequence >= 0
                                        ? reportDir / std::format("frame_{:05}.ppm", in.sequence)
                                        : reportDir / "frame.ppm";
        if (WritePpm(out, g_frame.data(), W, H))
            lucent::info("draw", "frame screenshot written to {}", out.string());
    }
    msReadback = sinceStartMs() - msSetup - msDrawLoop - msSubmit;
    // state+pipeline and record are supersets, so the line names their children
    // instead of inviting readers to add a meaningless flat list. Texture upload
    // belongs under record: TextureBinder::SelectView has its sole call inside
    // descriptor-write assembly. Shader translation is inside shader lookup and
    // pipeline creation is inside the following pipeline lookup.
    const double msStateOwn = msState - msModify - msShaderLookup - PC.msPipeline;
    // The census is INSIDE the prepare span (the viewport block), and the
    // descriptor update is inside the descriptor-write span, so neither is
    // subtracted here -- subtracting a child twice is how a residual goes
    // negative and gets waved away as a rounding artefact.
    const double msRecordOwn = msRecord - DB.msAlloc - DB.msWrite - msPrepare;
    const double msLoopOther = msDrawLoop - msState - msUniforms - msIndex - msRecord;
    lucent::Line frameCost, drawLoopCost;
    if (in.report)
    {
        frameCost.add("frame cost {:.0f} ms: setup {:.0f}, draw loop {:.0f},"
                      " guest-memory upload {:.0f}, submit+wait {:.0f}, readback+report {:.0f}",
                      sinceStartMs(), msSetup, msDrawLoop, msSsboUpload, msSubmit, msReadback);
        drawLoopCost.add("  draw loop {:.0f} ms = state+pipeline {:.0f}"
                         " (shader translation {:.0f} + pipeline creation {:.0f} + modification"
                         " derivation {:.0f} + shader/layout cache lookups {:.0f} + own {:.0f})"
                         " + uniforms {:.0f} + index prep {:.0f} + record {:.0f} (descriptor alloc"
                         " {:.0f} + descriptor writes {:.0f} of which texture upload {:.0f} and the"
                         " driver's update {:.0f}, so own {:.0f} + prepare {:.0f} of which viewport"
                         " census {:.0f} + own {:.0f}) + unattributed {:.0f}",
                         msDrawLoop, msState, SC.msTranslate, PC.msPipeline, msModify,
                         msShaderLookup, msStateOwn, msUniforms, msIndex, msRecord, DB.msAlloc,
                         DB.msWrite, TB.msUpload, DB.msUpdate,
                         DB.msWrite - TB.msUpload - DB.msUpdate, msPrepare, msCensus, msRecordOwn,
                         msLoopOther);
    }
    frameCost.flush(lucent::Level::Info, "draw");
    drawLoopCost.flush(lucent::Level::Info, "draw");

    // The draw loop rather than the whole frame, and not on report frames: their
    // readback and PPM cost would land on whichever arm they happened to use.
    //
    // AND NOT THE WARM-UP EITHER, which frame_ab.h says are "simply not recorded"
    // -- but that is the CALLER's job and this call site was not doing it. The
    // first render of a scene pays for every shader translation and every
    // pipeline in it, hundreds of milliseconds that appear in no later frame, and
    // including them inflates the arms' variance enough to bury a real effect:
    // an 81-frame replay reported the collapsed arm 15 ms faster and NOT RESOLVED
    // against a 30 ms noise floor. Skipping a fixed count cannot bias the result
    // because the arms keep alternating through it, so each loses the same number.
    static uint64_t renderedFrames = 0;
    ++renderedFrames;
    constexpr uint64_t kAbWarmupFrames = 12;
    const bool recordable =
        FrameMayRecordMeasurement(in.report, in.probe) && renderedFrames > kAbWarmupFrames;
    if (abUntile.Enabled() && recordable)
        abUntile.RecordFrame(msDrawLoop);
    if (ab.Enabled() && recordable)
        ab.RecordFrame(msDrawLoop);
    if (abTexDirty.Enabled() && recordable)
        abTexDirty.RecordFrame(msDrawLoop);
    // One reporter for whichever A/B is running. Both arms are summarised the
    // same way, including the case it must not get wrong: a difference smaller
    // than the run's own resolution is noise and is printed as such.
    auto summarise = [&](AbTest &t, const char *what)
    {
        if (!t.Enabled() || !in.report)
            return;
        AbSummary sm;
        if (!t.Summarise(sm))
        {
            lucent::info("draw",
                         "A/B ({}): nothing recorded yet -- every frame so"
                         " far was a report frame, which is excluded",
                         what);
            return;
        }
        if (sm.resolved)
        {
            lucent::info("draw",
                         "A/B ({}): the experimental arm is {:+.2f} ms"
                         " ({:.2f} vs {:.2f} ms over {} and {} frames), and that is"
                         " larger than the {:.2f} ms this run can resolve",
                         what, sm.differenceMs, sm.armMs, sm.baselineMs, sm.armFrames,
                         sm.baselineFrames, sm.noiseMs);
        }
        else
        {
            // THE NEGATIVE, WITH ITS DENOMINATOR. "No difference" from a run that
            // could not have seen one is not a measurement, so the resolution is
            // printed next to the difference every time.
            lucent::info("draw",
                         "A/B ({}): NOT RESOLVED. The arms differ by"
                         " {:+.2f} ms ({:.2f} vs {:.2f} over {} and {} frames) but this run"
                         " can only resolve {:.2f} ms, so that number is noise -- do not"
                         " read it as a small effect in either direction",
                         what, sm.differenceMs, sm.armMs, sm.baselineMs, sm.armFrames,
                         sm.baselineFrames, sm.noiseMs);
        }
    };
    summarise(ab, "per-draw viewport census");
    summarise(abUntile, "EDRAM tiling collapsed");
    summarise(abTexDirty, "texture staleness page-skips");
    PB.Report(prepared);

    // --- teardown --------------------------------------------------------
    PB.Release();
    // Only this frame's own transients are destroyed here. The render target,
    // passes, layouts, pipelines, shader modules, textures and samplers belong
    // to RendererPersistent and are released by ReleasePersistent.
    for (size_t i = 0; i < stagingBufs.size(); ++i)
    {
        vkDestroyBuffer(device, stagingBufs[i], nullptr);
        vkFreeMemory(device, stagingMems[i], nullptr);
    }
    AR.Release();
    // Textures evicted this frame because the guest overwrote their bytes. The
    // fence above has been waited on, so no draw of this frame still references
    // them; they were kept alive until here precisely because earlier draws did.
    for (GuestTex &t : TX.texRetired)
    {
        vkDestroyImageView(device, t.view, nullptr);
        vkDestroyImage(device, t.image, nullptr);
        vkFreeMemory(device, t.mem, nullptr);
    }
    P.built = true;
    return true;
}

} // namespace draw

using draw::kHeight;
using draw::kWidth;
using draw::Renderer;

// The renderer is built once and kept. Rebuilding it per frame was what made a
// frame cost ~300 ms; the device, render target, shader translations, pipelines
// and textures all survive from one frame to the next now.
Renderer &FrameRenderer()
{
    static Renderer r;
    static bool initialised = r.Init();
    (void)initialised;
    return r;
}

bool RenderFrame(const FrameDrawInputs &in)
{
    Renderer &r = FrameRenderer();
    if (r.device == VK_NULL_HANDLE)
    {
        draw::g_frame.clear();
        return false;
    }
    const bool ok = r.RenderFrameImpl(in);
    if (!ok)
        draw::g_frame.clear();
    return ok;
}

void ResetRendererForComparison()
{
    Renderer &r = FrameRenderer();
    if (r.device != VK_NULL_HANDLE)
    {
        SharedGpuQueueAccess().WaitDeviceIdle(r.device);
        r.ReleasePersistent();
    }
}

const std::vector<uint8_t> &GuestFramePixels()
{
    return draw::g_frame;
}
uint32_t GuestFrameWidth()
{
    return kWidth;
}
uint32_t GuestFrameHeight()
{
    return kHeight;
}

} // namespace gears

#else // GEARS_HAVE_GUEST_DRAW

namespace gears
{
bool RenderFrame(const FrameDrawInputs &)
{
    lucent::warn("draw", "built without the guest-draw backend"
                         " (needs Vulkan + the Xenos translator)");
    return false;
}
const std::vector<uint8_t> &GuestFramePixels()
{
    static std::vector<uint8_t> empty;
    return empty;
}
uint32_t GuestFrameWidth()
{
    return 0;
}
uint32_t GuestFrameHeight()
{
    return 0;
}
} // namespace gears

#endif
