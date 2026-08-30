// Host graphics backend: window, Vulkan swapchain, present.
//
// SCOPE. This translation unit puts a window on screen and flips it once per
// guest frame. It owns the window, the swapchain and the present queue and
// nothing else: the pixels come from the guest-draw backend (gpu_draw.cpp),
// which renders every draw of the frame and publishes the resulting image on
// the shared device for this file to blit into the swapchain. A frame that has
// not been drawn yet presents black -- there is no substitute content, because
// a substitute is indistinguishable from the renderer working.
//
// WHERE PRESENT COMES FROM. Not a host timer. gears::PresentFrame is called by
// the command processor when it executes an accepted swap packet -- the packet
// the kernel's VdSwap writes into D3D's 64-dword reservation, carrying a
// sequence number so that stale re-submitted copies are skipped. So one present
// happens per guest VdSwap, at the point in the command stream where the
// hardware would have flipped. If the guest stops swapping, the window stops
// updating.
//
// THREADING. The runtime's main thread enters the guest at _xstart and never
// comes back, so it cannot own a window. A dedicated thread owns SDL, the
// window, the event pump and every Vulkan object; nothing else touches them.
// The command processor hands over a request and waits for it to complete, so
// the cost of presenting lands in the guest's measured frame rate instead of
// hiding behind a queue.
//
// HEADLESS. Every measurement harness in this project runs without a display.
// If SDL cannot start video, or there is no Vulkan device, or the surface
// cannot be created, the presenter reports why at warn level and stays off; the
// runtime then behaves exactly as it did before. GEARS_NO_WINDOW=1 forces that
// path deliberately.
#include "gpu_present.h"

#include <cstdio>
#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <format>
#include <vector>

#include <lucent/config.h>
#include <lucent/log.h>

#include "gpu_device_features.h"
#include "host_product_identity.h"
#include "gpu_present_stage.h"
#include "gpu_present_source.h"
#include "gpu_shared_device.h"
#include "gpu_queue_family.h"
#include "gpu_queue_access.h"
#include "swapchain_format.h"
#ifdef GEARS_HAVE_PRESENTER

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "input.h"

namespace
{

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;

const char *ResultName(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR:
        return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR:
        return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR:
        return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    default:
        return "VkResult";
    }
}

struct Presenter
{
    static constexpr uint32_t kInFlight = gears::PresentSourceSlots::kCapacity;

    // --- host objects, touched only by the present thread -------------------
    SDL_Window *window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t lastBlittedFrame = 0;
    uint64_t blankPresents = 0;
    uint64_t freshFramesShown = 0;
    uint64_t repeatedFramesShown = 0;

    // Presented-image readback is independent from the upload path it checks.
    VkBuffer presentCapture = VK_NULL_HANDLE;
    VkDeviceMemory presentCaptureMem = VK_NULL_HANDLE;
    void *presentCaptureMapped = nullptr;
    VkDeviceSize presentCaptureBytes = 0;
    uint64_t presentCaptureWanted = 0; // how many frames still to write
    uint64_t presentCaptureAfter = 0;  // ...but not before this present

    VkBuffer guestStaging = VK_NULL_HANDLE;
    VkDeviceMemory guestStagingMem = VK_NULL_HANDLE;
    void *guestStagingMapped = nullptr;
    VkDeviceSize guestStagingSize = 0;

    VkCommandPool commandPool = VK_NULL_HANDLE;

    // --- measurement --------------------------------------------------------
    uint64_t presentCount = 0;
    uint64_t presentMicros = 0;
    std::chrono::steady_clock::time_point lastReport;

    // THE SWAPCHAIN IS TAGGED sRGB, AND THE FRAME GOES IN AS RAW BYTES.
    //
    // A UNORM swapchain says nothing about its transfer function, and a compositor
    // is free to read the bytes as linear light and encode them for the display.
    // Measured on this machine: the window's 25th and 50th percentiles were
    // sRGB_encode() of the frame we presented, to three decimals. Our bytes ARE
    // sRGB-encoded -- the guest tonemapped them -- so the honest thing is to say so,
    // and a *_SRGB swapchain format is how that is said.
    //
    // The catch is that a blit into sRGB encodes a second time. The frame is
    // therefore blitted into the matching UNORM component layout, then copied as
    // raw bytes. The stage format is derived from the swapchain; hardcoding BGRA
    // made an RGBA-only surface exchange red and blue across the whole window.
    gears::SrgbRawCopyStage srgbStage;
    VkCommandBuffer commands[kInFlight]{};
    VkSemaphore acquired[kInFlight]{};
    VkFence submitted[kInFlight]{};
    gears::PresentSourceSlots sources;
    std::vector<VkImage> images;
    // One semaphore per swapchain image: a present-wait semaphore stays in use
    // until the presentation engine is done with that image.
    std::vector<VkSemaphore> presentReady;

    // --- handshake with the command processor -------------------------------
    std::mutex mutex;
    std::condition_variable request;
    std::condition_variable done;
    VkPhysicalDeviceMemoryProperties memProps{};

    uint32_t queueFamily = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t frameSlot = 0;
    uint32_t pendingSequence = 0;
    uint32_t presentedFrameSamples = 0;
    uint32_t presentedFrameBestP99 = 0;
    VkExtent2D extent{};

    // These flags are kept together after the naturally aligned state above so
    // Presenter does not spend cache space on alignment holes between them.
    bool canCapturePresented = false;
    bool headlessSurface = false;
    bool announcedBlit = false;
    bool announcedBlank = false;
    bool swapchainIsSrgb = false;
    bool pending = false;
    bool serviced = false;
    std::atomic<bool> running{false};
    std::atomic<bool> shuttingDown{false};
    bool presentedFrameChecked = false;

    bool Start();
    void Stop();
    void Thread();

    bool CreateInstanceAndDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool EnsureGuestStaging(VkDeviceSize size);
    // The presented-frame capture. Separate from the guest staging buffer because
    // this one is read by the CPU rather than written by it, and because a
    // diagnostic must not share a resource with the path it is checking.
    bool EnsurePresentCapture(VkDeviceSize size);
    void WritePresentedFrame(uint32_t width, uint32_t height, VkFormat format);
    // One look at the numbers of a real presented frame, to catch the frame
    // being finished-looking-but-not-finished. Runs once per run.
    void CheckPresentedFrameLooksFinished(uint32_t width, uint32_t height, VkFormat format);
    bool PresentOne(uint32_t sequence);
    void PumpEvents();
};

Presenter g_presenter;

bool Presenter::CreateInstanceAndDevice()
{
    // A REAL SWAPCHAIN WITH NO WINDOW, when asked for.
    //
    // Everything this project measures is taken from the renderer's readback,
    // which is BEFORE the blit into the swapchain -- so the present path, the one
    // step that decides what a person actually sees, was outside every instrument
    // here. It could only be checked by opening a window, and a window is not
    // something a measurement run may do on someone's desktop.
    //
    // VK_EXT_headless_surface removes the dilemma: a surface backed by nothing,
    // with a real swapchain, real images, the real blit and a real vkQueuePresent.
    // GEARS_PRESENT_HEADLESS=1 takes that path, and GEARS_PRESENT_DUMP then
    // captures what would have reached the screen.
    std::vector<const char *> extensions;
    if (headlessSurface)
    {
        uint32_t available = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &available, nullptr);
        std::vector<VkExtensionProperties> props(available);
        vkEnumerateInstanceExtensionProperties(nullptr, &available, props.data());
        bool have = false;
        for (const auto &e : props)
            if (std::strcmp(e.extensionName, "VK_EXT_headless_surface") == 0)
                have = true;
        if (!have)
        {
            lucent::error("present",
                          "GEARS_PRESENT_HEADLESS=1 but this loader has no"
                          " VK_EXT_headless_surface, so there is no way to exercise the present"
                          " path without a window. Refusing rather than silently presenting"
                          " nothing");
            return false;
        }
        extensions.push_back("VK_KHR_surface");
        extensions.push_back("VK_EXT_headless_surface");
    }
    else
    {
        uint32_t sdlExtensionCount = 0;
        const char *const *sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
        if (sdlExtensions == nullptr)
        {
            lucent::warn("present", "SDL_Vulkan_GetInstanceExtensions: {}", SDL_GetError());
            return false;
        }
        extensions.assign(sdlExtensions, sdlExtensions + sdlExtensionCount);
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = gears::kHostProductName;
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = uint32_t(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();

    VkResult r = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (r != VK_SUCCESS)
    {
        lucent::warn("present", "vkCreateInstance failed ({})", ResultName(r));
        instance = VK_NULL_HANDLE;
        return false;
    }

    if (headlessSurface)
    {
        auto create = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(
            instance, "vkCreateHeadlessSurfaceEXT");
        VkHeadlessSurfaceCreateInfoEXT hs{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        if (create == nullptr || create(instance, &hs, nullptr, &surface) != VK_SUCCESS)
        {
            lucent::error("present", "vkCreateHeadlessSurfaceEXT failed");
            return false;
        }
        lucent::info("present", "headless surface created: the real swapchain, blit"
                                " and present run with no window");
    }
    else if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
    {
        lucent::warn("present", "SDL_Vulkan_CreateSurface: {}", SDL_GetError());
        return false;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (deviceCount != 0)
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    // Prefer a discrete GPU, but require only what is actually needed: a queue
    // family that both presents to this surface and accepts graphics work.
    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices)
    {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());

        // The policy lives in gpu_queue_family.h, shared with the draw path so the
        // two cannot drift -- they each had their own loop checking only their own
        // half of the requirement. It also refuses a family advertising zero
        // queues, which this loop did not.
        std::vector<gears::QueueFamily> capabilities(familyCount);
        for (uint32_t i = 0; i < familyCount; i++)
        {
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported);
            capabilities[i].graphics = (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            capabilities[i].present = supported == VK_TRUE;
            capabilities[i].count = families[i].queueCount;
        }

        const uint32_t chosen = gears::ChooseQueueFamily(capabilities, /*needPresent=*/true);
        if (chosen == gears::kNoQueueFamily)
            continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);
        const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 2
                          : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                       : 0;
        if (score > bestScore)
        {
            bestScore = score;
            physical = candidate;
            queueFamily = chosen;
        }
    }

    if (physical == VK_NULL_HANDLE)
    {
        lucent::warn("present",
                     "no Vulkan device can present to this surface"
                     " ({} device(s) enumerated)",
                     deviceCount);
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char *deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    // THE RENDERER'S FEATURES TOO, because this is the device the draw path adopts
    // rather than creating a second one. Two devices costs a readback of every
    // rendered frame to host memory and a staging upload back, purely because the
    // image and the swapchain would live on different devices. Creating this one
    // without the renderer's features would hand it a device missing the geometry
    // shader its rectangle lists need and the unformatted storage images its resolve
    // pass needs -- undefined behaviour rather than a clean failure.
    VkPhysicalDeviceFeatures features{};
    gears::DeviceCapabilities capabilities{};
    gears::SelectDeviceFeatures(physical, features, capabilities);

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    deviceInfo.pEnabledFeatures = &features;

    r = vkCreateDevice(physical, &deviceInfo, nullptr, &device);
    if (r != VK_SUCCESS)
    {
        lucent::warn("present", "vkCreateDevice failed ({})", ResultName(r));
        device = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    // Publish for the draw path to adopt. This side publishes because it comes up
    // first in a windowed run -- the presenter thread starts at the first guest
    // swap, the draw path at GEARS_DRAW_FRAME_AT -- and because only this side has a
    // surface, so only this side can choose a queue family verified to present.
    {
        gears::SharedGpu shared;
        shared.instance = instance;
        shared.physical = physical;
        shared.device = device;
        shared.queue = queue;
        shared.queueFamily = queueFamily;
        shared.checkedForPresent = true;
        gears::PublishSharedGpu(shared);
    }

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kInFlight;
    if (vkAllocateCommandBuffers(device, &allocInfo, commands) != VK_SUCCESS)
        return false;

    for (uint32_t i = 0; i < kInFlight; i++)
    {
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &submitted[i]) != VK_SUCCESS)
            return false;
    }

    lucent::info("present", "Vulkan device \"{}\" (queue family {})", props.deviceName,
                 queueFamily);
    return true;
}

bool Presenter::CreateSwapchain()
{
    VkSurfaceCapabilitiesKHR caps{};
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps);
    if (r != VK_SUCCESS)
    {
        lucent::warn("present", "vkGetPhysicalDeviceSurfaceCapabilitiesKHR ({})", ResultName(r));
        return false;
    }

    extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu)
    {
        extent.width = kWindowWidth;
        extent.height = kWindowHeight;
    }
    if (extent.width == 0 || extent.height == 0)
        return false; // minimised; try again on the next frame

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &formatCount, formats.data());
    if (formats.empty())
        return false;

    // EVERY PAIR THE SURFACE OFFERS, once, before choosing. Which format/colour
    // space a display exposes is the one input to this decision that differs
    // between machines, and it was never in the log -- so a window that looks wrong
    // on someone else's desktop could not be diagnosed from their run.
    {
        lucent::Line fl;
        fl.add("surface offers {} format/colour-space pair(s):", formats.size());
        for (const VkSurfaceFormatKHR &f : formats)
            fl.add(" [{}/{}]", uint32_t(f.format), uint32_t(f.colorSpace));
        fl.add(" (colour space 0 is SRGB_NONLINEAR, which is the one to want;"
               " 1000104002 is EXTENDED_SRGB_LINEAR and 1000104008 is HDR10_ST2084,"
               " both of which re-interpret an already-encoded frame)");
        fl.flush(lucent::Level::Info, "present");
    }

    // THE SWAPCHAIN MUST NOT BE AN sRGB FORMAT.
    //
    // The drawn frame is R8G8B8A8_UNORM holding bytes the guest already tonemapped
    // -- display-ready values. vkCmdBlitImage between formats CONVERTS, so an sRGB
    // swapchain image makes the driver treat those bytes as linear and encode them:
    // mid-tones lift hard, blacks stay black, and the window shows a flat washed-out
    // version of a frame the renderer produced correctly.
    //
    // The selection lives in swapchain_format.h as a pure function with tests,
    // because this decides every pixel of the window and no capture in this project
    // can see it -- the renderer's screenshots come from its readback, before the
    // blit. The old code preferred B8G8R8A8_UNORM with SRGB_NONLINEAR and fell back
    // to formats[0], which is an sRGB format on this driver; that fallback only
    // bites on a surface that does not offer the preferred pair, so WHICH format a
    // given surface yields is now logged rather than assumed.
    // PREFER THE sRGB TAG, now that the frame can reach it unconverted. The bytes
    // are sRGB-encoded and a *_SRGB format says exactly that, which leaves a
    // compositor nothing to assume. GEARS_PRESENT_UNORM=1 selects the old
    // untagged arrangement, as the control arm for a display where this is wrong.
    const VkSurfaceFormatKHR chosen = gears::ChooseSwapchainFormat(
        formats.data(), formats.size(), !lucent::config::flag("PRESENT_UNORM"));
    format = chosen.format;
    if (gears::SwapchainFormatIsSrgb(format))
        lucent::warn("present",
                     "this surface offers no non-sRGB format, so the"
                     " swapchain is {} and the blit will RE-ENCODE the already-tonemapped"
                     " frame to sRGB: the window will look washed out and flat. The frame"
                     " itself is correct -- compare the renderer's own screenshot",
                     uint32_t(format));
    else
        lucent::info("present",
                     "swapchain format {} (non-sRGB, so the blit copies"
                     " the drawn frame's bytes without a colour-space conversion)",
                     uint32_t(format));

    // THE COLOUR SPACE MATTERS AS MUCH AS THE FORMAT, and only the format was
    // reported. A UNORM format paired with EXTENDED_SRGB_LINEAR or an HDR10 space
    // tells the compositor to read our already-sRGB-encoded bytes as linear light
    // or as PQ, and it will map them for the display: mid-tones lift, contrast
    // flattens, and the window looks washed out with a frame that is byte-perfect
    // all the way to vkQueuePresent. That is indistinguishable, from inside this
    // process, from the frame being right -- because it IS right.
    if (chosen.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        lucent::warn("present",
                     "swapchain colour space is {} rather than"
                     " SRGB_NONLINEAR: the compositor will re-interpret the frame's bytes"
                     " and the window can look washed out even though every pixel handed to"
                     " the swapchain is correct",
                     uint32_t(chosen.colorSpace));
    else
        lucent::info("present", "swapchain colour space SRGB_NONLINEAR (the"
                                " compositor takes the bytes as sRGB, which is what they are)");

    // FIFO is the only mode the spec guarantees, but it would make every
    // present block on the host's 60 Hz refresh -- a host clock leaking into
    // the guest's frame loop, which is exactly what this milestone must not
    // do. MAILBOX presents immediately and still never tears, so it is
    // preferred; FIFO remains the fallback and is reported when used.
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &modeCount, modes.data());
    presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR mode : modes)
    {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
        {
            presentMode = mode;
            break;
        }
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface;
    info.minImageCount = imageCount;
    info.imageFormat = format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    // TRANSFER_DST because the frame is produced with vkCmdClearColorImage or a
    // copy from the guest staging buffer: no render pass, no pipeline, no shaders.
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    // AND TRANSFER_SRC WHEN THE SURFACE ALLOWS IT, so what was actually presented
    // can be read back and written out. Without this there is no way to check what
    // reaches the window: the draw path's screenshots come from its own readback,
    // which proves what it RENDERED, not what the presenter put on screen. The next
    // change here removes that readback and blits the drawn image straight into the
    // swapchain, and "it did not crash" is not verification of a change that can go
    // subtly wrong in colour, orientation or extent.
    canCapturePresented = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (canCapturePresented)
        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = swapchain;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(device, &info, nullptr, &created);
    if (r != VK_SUCCESS)
    {
        lucent::warn("present", "vkCreateSwapchainKHR failed ({})", ResultName(r));
        return false;
    }

    DestroySwapchain();
    swapchain = created;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr);
    images.resize(count);
    vkGetSwapchainImagesKHR(device, swapchain, &count, images.data());

    swapchainIsSrgb = gears::SwapchainFormatIsSrgb(format);
    if (swapchainIsSrgb && !srgbStage.Create(device, memProps, extent, format))
        lucent::warn("present", "no raw-copy stage; the frame will be blitted"
                                " into the sRGB swapchain and encoded twice");

    presentReady.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &presentReady[i]);
    }

    lucent::info("present", "swapchain {}x{}, {} images, {}", extent.width, extent.height, count,
                 presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO (vsync-paced)");
    return true;
}

void Presenter::DestroySwapchain()
{
    srgbStage.Destroy(device);
    for (VkSemaphore s : presentReady)
        if (s != VK_NULL_HANDLE)
            vkDestroySemaphore(device, s, nullptr);
    presentReady.clear();
    images.clear();
    // The old swapchain handle is retired by the caller passing it as
    // oldSwapchain, but it still has to be destroyed once the new one exists.
    if (swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}

bool Presenter::EnsurePresentCapture(VkDeviceSize size)
{
    if (presentCapture != VK_NULL_HANDLE && presentCaptureBytes >= size)
        return true;
    if (presentCapture != VK_NULL_HANDLE)
    {
        if (presentCaptureMapped)
            vkUnmapMemory(device, presentCaptureMem);
        vkDestroyBuffer(device, presentCapture, nullptr);
        vkFreeMemory(device, presentCaptureMem, nullptr);
        presentCapture = VK_NULL_HANDLE;
        presentCaptureMapped = nullptr;
    }

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, nullptr, &presentCapture) != VK_SUCCESS)
        return false;

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, presentCapture, &req);
    uint32_t type = UINT32_MAX;
    // HOST_CACHED as well as visible: the CPU READS this one, and an uncached read
    // of a whole frame is far slower than the copy that filled it.
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & want) == want)
        {
            type = i;
            break;
        }
    if (type == UINT32_MAX)
    {
        vkDestroyBuffer(device, presentCapture, nullptr);
        presentCapture = VK_NULL_HANDLE;
        lucent::warn("present", "no host-visible memory for a presented-frame"
                                " capture, so none can be written");
        return false;
    }

    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &ai, nullptr, &presentCaptureMem) != VK_SUCCESS ||
        vkBindBufferMemory(device, presentCapture, presentCaptureMem, 0) != VK_SUCCESS ||
        vkMapMemory(device, presentCaptureMem, 0, req.size, 0, &presentCaptureMapped) != VK_SUCCESS)
    {
        lucent::warn("present", "could not map a presented-frame capture buffer");
        return false;
    }
    presentCaptureBytes = size;
    return true;
}

// Does the frame on its way to the screen look like a FINISHED frame?
//
// A reported symptom this project could never reproduce: the window showing flat
// grey, every texture detail present, no lighting contrast, everything squeezed
// into the bottom third of the range. That is what a pre-tonemap linear-light
// buffer looks like presented as if it were display-ready -- and from inside the
// runtime it is indistinguishable from a correct dark scene unless somebody looks
// at the numbers.
//
// So the numbers get looked at, once, on a real presented frame: the 99th
// percentile of luminance. A finished frame has highlights -- UI text, a sky, a
// specular -- and reaches near the top of the range. A linear-light buffer does
// not. The threshold is deliberately low so a genuinely dark scene does not trip
// it; the case this is for sits at 0.30.
void Presenter::CheckPresentedFrameLooksFinished(uint32_t width, uint32_t height, VkFormat format)
{
    if (presentCaptureMapped == nullptr || presentedFrameChecked)
        return;
    const bool bgr = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    const uint8_t *px = static_cast<const uint8_t *>(presentCaptureMapped);
    uint64_t histogram[256] = {};
    const uint64_t pixels = uint64_t(width) * height;
    for (uint64_t i = 0; i < pixels; ++i)
    {
        const uint8_t r = px[i * 4 + (bgr ? 2 : 0)];
        const uint8_t g = px[i * 4 + 1];
        const uint8_t b = px[i * 4 + (bgr ? 0 : 2)];
        ++histogram[std::max({r, g, b})];
    }
    uint64_t seen = 0;
    uint32_t p99 = 0;
    for (uint32_t v = 0; v < 256; ++v)
    {
        seen += histogram[v];
        if (seen >= pixels * 99 / 100)
        {
            p99 = v;
            break;
        }
    }
    // KEEP LOOKING until a frame has highlights, rather than judging the first one
    // sampled. The title's opening minute is legitimately black -- logos fading in,
    // a dark cell -- so one flat sample proves nothing. A run that never produces a
    // frame with highlights, across samples spanning a couple of minutes, is the
    // case worth a warning.
    ++presentedFrameSamples;
    presentedFrameBestP99 = std::max(presentedFrameBestP99, p99);
    if (p99 >= 90)
    {
        presentedFrameChecked = true;
        lucent::info("present",
                     "presented frame checked after {} sample(s):"
                     " 99th-percentile brightness {}/255, so what reaches the screen has"
                     " highlights and is not an untonemapped buffer",
                     presentedFrameSamples, p99);
    }
    else if (presentedFrameSamples >= 12)
    {
        presentedFrameChecked = true;
        lucent::warn("present",
                     "{} presented frames sampled over this run and the"
                     " brightest 99th percentile any of them reached was {}/255: nothing that"
                     " reaches the screen has highlights. A finished frame reaches the top of"
                     " the range somewhere -- UI text, a light, a sky. This is what an"
                     " untonemapped linear-light buffer looks like presented as display-ready;"
                     " check the presented surface against the guest's front-buffer address in"
                     " the draw log",
                     presentedFrameSamples, presentedFrameBestP99);
    }
}

void Presenter::WritePresentedFrame(uint32_t width, uint32_t height, VkFormat format)
{
    if (presentCaptureMapped == nullptr)
        return;

    // The swapchain is BGRA on every driver seen here, but the format is passed in
    // rather than assumed: writing the channels the wrong way round would make a
    // correct blit look broken, which is worse than not capturing at all.
    const bool bgr = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;

    const std::string &dirText = lucent::config::text("PRESENT_DUMP_DIR");
    const std::filesystem::path dir = dirText.empty() ? std::filesystem::path("scratch/screenshots")
                                                      : std::filesystem::path(dirText);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path out = dir / std::format("presented_{}.ppm", presentCaptureWanted);

    std::FILE *f = std::fopen(out.string().c_str(), "wb");
    if (f == nullptr)
    {
        lucent::warn("present", "could not open {} for the presented frame", out.string());
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", width, height);
    const uint8_t *src = static_cast<const uint8_t *>(presentCaptureMapped);
    uint64_t nonBlack = 0;
    std::set<std::array<uint8_t, 3>> distinct;
    std::vector<uint8_t> row(size_t(width) * 3);
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t *p = src + (size_t(y) * width + x) * 4;
            const uint8_t r = bgr ? p[2] : p[0];
            const uint8_t g = p[1];
            const uint8_t b = bgr ? p[0] : p[2];
            row[size_t(x) * 3 + 0] = r;
            row[size_t(x) * 3 + 1] = g;
            row[size_t(x) * 3 + 2] = b;
            if (r || g || b)
                ++nonBlack;
            if (distinct.size() < 4096)
                distinct.insert({r, g, b});
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);

    // DISTINCT COLOURS, not just non-black. The first version reported "921600/921600
    // px non-black (100.0%)" for a capture that held a single flat red -- the
    // presenter's clear colour, because the capture fired before the draw path had
    // produced anything. Non-black is satisfied by any clear, so on its own it says
    // almost nothing; a count of 1 says the frame is flat, and that is the reading
    // that matters when checking whether a real rendered frame reached the window.
    lucent::info("present",
                 "presented frame written to {} -- {}/{} px non-black"
                 " ({:.1f}%), {} distinct colour(s){}",
                 out.string(), nonBlack, uint64_t(width) * height,
                 100.0 * double(nonBlack) / (double(width) * height),
                 // The counter stops at 4096 so a busy frame does not cost a set insertion
                 // per pixel forever; say so, or the number reads as an exact count that
                 // suspiciously lands on a power of two.
                 distinct.size() >= 4096 ? std::string("4096+ (counter capped)")
                                         : std::to_string(distinct.size()),
                 distinct.size() <= 2
                     ? "  <- FLAT, so this is a clear colour and not a rendered frame"
                     : "");
}

bool Presenter::EnsureGuestStaging(VkDeviceSize size)
{
    if (guestStaging != VK_NULL_HANDLE && guestStagingSize >= size)
        return true;
    if (guestStaging != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(device, guestStaging, nullptr);
        vkFreeMemory(device, guestStagingMem, nullptr);
        guestStaging = VK_NULL_HANDLE;
        guestStagingMapped = nullptr;
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bi, nullptr, &guestStaging) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, guestStaging, &req);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & want) == want)
        {
            type = i;
            break;
        }
    if (type == UINT32_MAX)
    {
        vkDestroyBuffer(device, guestStaging, nullptr);
        guestStaging = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &ai, nullptr, &guestStagingMem) != VK_SUCCESS ||
        vkBindBufferMemory(device, guestStaging, guestStagingMem, 0) != VK_SUCCESS ||
        vkMapMemory(device, guestStagingMem, 0, req.size, 0, &guestStagingMapped) != VK_SUCCESS)
    {
        vkDestroyBuffer(device, guestStaging, nullptr);
        guestStaging = VK_NULL_HANDLE;
        return false;
    }
    guestStagingSize = req.size;
    return true;
}

bool Presenter::PresentOne(uint32_t sequence)
{
    if (swapchain == VK_NULL_HANDLE && !CreateSwapchain())
        return false;

    const uint32_t slot = frameSlot;
    frameSlot = (frameSlot + 1) % kInFlight;

    if (!sources.Begin(device, submitted[slot], slot))
    {
        gears::SharedGpuQueueAccess().WaitDeviceIdle(device);
        sources.ResetAfterDeviceIdle();
        return false;
    }

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[slot],
                                       VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // The window was resized between frames. Rebuild and drop this frame
        // rather than presenting into a stale surface; the guest's next VdSwap
        // brings the next one.
        gears::SharedGpuQueueAccess().WaitDeviceIdle(device);
        sources.ResetAfterDeviceIdle();
        return CreateSwapchain();
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        lucent::warn("present", "vkAcquireNextImageKHR ({})", ResultName(r));
        return false;
    }

    vkResetFences(device, 1, &submitted[slot]);
    vkResetCommandBuffer(commands[slot], 0);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands[slot], &begin);

    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    VkImageMemoryBarrier toClear{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toClear.srcAccessMask = 0;
    toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toClear.image = images[imageIndex];
    toClear.subresourceRange = range;
    vkCmdPipelineBarrier(commands[slot], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toClear);

    // The frame the guest-draw backend rendered, and nothing else. Two ways in:
    // a blit from the drawn image when the draw path shares this device, or a
    // copy through a staging buffer when it does not. Until the first frame is
    // drawn the swapchain image is cleared to black -- see the clear below.
    bool uploadedGuest = false;
    gears::SharedFrameImage::Lease drawnLease;

    // PREFER A BLIT FROM THE DRAWN IMAGE. When the draw path adopted this device it
    // publishes the image it rendered into, already in TRANSFER_SRC_OPTIMAL and
    // already waited on, so the frame can go straight to the swapchain -- no readback
    // to host memory, no staging buffer, no per-pixel copy on the CPU.
    //
    // The red/blue swap the upload path below performs is NOT needed here: that swap
    // exists because a CPU memcpy into a BGRA image is byte-order sensitive, whereas
    // vkCmdBlitImage converts between formats component by component. If the colours
    // come out swapped, this comment is wrong and GEARS_PRESENT_DUMP will show it.
    {
        auto drawn = sources.RecordLatest(commands[slot], images[imageIndex], extent,
                                          swapchainIsSrgb, srgbStage);
        if (drawn.recorded)
        {
            if (drawn.sequence != lastBlittedFrame)
            {
                ++freshFramesShown;
                lastBlittedFrame = drawn.sequence;
            }
            else
            {
                // The same frame, shown again. Counted, because "the renderer is
                // keeping up" and "the window is holding one frame for four flips"
                // look identical from outside and sound identical in a bug report.
                ++repeatedFramesShown;
            }
            uploadedGuest = true;
            drawnLease = std::move(drawn.lease);
            if (!announcedBlit)
            {
                announcedBlit = true;
                // Precisely what is bypassed, and what is not. The staging
                // buffer, the per-pixel CPU red/blue swap and the buffer-to-image
                // upload are gone. The DRAW side still copies the frame into its own
                // readback buffer, which now feeds only its diagnostics -- claiming
                // otherwise would credit this change with a cost it has not removed.
                lucent::info("present",
                             "presenting the drawn image by BLIT on the"
                             " shared device: the staging buffer, the CPU red/blue swap, the"
                             " buffer-to-image upload and the draw side's frame readback are"
                             " all bypassed (the readback still runs on reported frames, where"
                             " its diagnostics need the pixels)");
            }
        }
    }

    const std::vector<uint8_t> &guest = gears::GuestFramePixels();
    const uint32_t gw = gears::GuestFrameWidth();
    const uint32_t gh = gears::GuestFrameHeight();
    if (!uploadedGuest && !guest.empty() && gw == extent.width && gh == extent.height &&
        EnsureGuestStaging(VkDeviceSize(guest.size())))
    {
        uint8_t *dst = static_cast<uint8_t *>(guestStagingMapped);
        for (size_t i = 0; i < guest.size(); i += 4)
        {
            dst[i + 0] = guest[i + 2]; // B
            dst[i + 1] = guest[i + 1]; // G
            dst[i + 2] = guest[i + 0]; // R
            dst[i + 3] = guest[i + 3]; // A
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyBufferToImage(commands[slot], guestStaging, images[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        uploadedGuest = true;
    }
    if (!uploadedGuest)
    {
        // No drawn frame for this swap: present black. A swapchain image is
        // acquired in UNDEFINED layout, so something must write it or the window
        // shows whatever was in that memory.
        //
        // Say so, rather than presenting a colour and letting the window look
        // alive: a black window and a window presenting substitute content are
        // indistinguishable from the outside, and the second one lies. Counted
        // and reported once, because the early frames of every run legitimately
        // land here -- the guest swaps before the renderer has a frame.
        VkClearColorValue black{};
        black.float32[3] = 1.0f;
        vkCmdClearColorImage(commands[slot], images[imageIndex],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        ++blankPresents;
        if (!announcedBlank)
        {
            announcedBlank = true;
            lucent::info("present",
                         "presenting BLACK: the guest swapped (sequence {})"
                         " before the draw backend published a frame. Every present until the"
                         " first drawn frame is black, and the count is reported at shutdown.",
                         sequence);
        }
    }

    // CAPTURE WHAT IS ABOUT TO BE PRESENTED, when asked. The image is in
    // TRANSFER_DST here, so it goes to TRANSFER_SRC for the copy and on to
    // PRESENT_SRC afterwards. Only when a capture was requested AND the surface
    // permits TRANSFER_SRC -- otherwise the swapchain was created without it and
    // the copy would be invalid.
    // ONE SELF-CHECK PER RUN, unasked. The dump below is opt-in and needs a flag
    // nobody remembers; this reads a single real presented frame, once, a few
    // hundred presents in (by which time the title is past its black startup), and
    // says whether what reaches the screen has any highlights in it. It is the only
    // thing in the runtime that looks at what a person actually sees.
    const bool selfCheckThisFrame =
        !presentedFrameChecked && presentCount > 300 && (presentCount % 300) == 0 &&
        uploadedGuest && canCapturePresented &&
        EnsurePresentCapture(VkDeviceSize(extent.width) * extent.height * 4);
    const bool capturingThisFrame =
        (presentCaptureWanted > 0 && sequence >= presentCaptureAfter && canCapturePresented &&
         EnsurePresentCapture(VkDeviceSize(extent.width) * extent.height * 4)) ||
        selfCheckThisFrame;
    if (capturingThisFrame)
    {
        VkImageMemoryBarrier toSrc = toClear;
        toSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(commands[slot], VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

        VkBufferImageCopy grab{};
        grab.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        grab.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(commands[slot], images[imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, presentCapture, 1, &grab);
    }

    VkImageMemoryBarrier toPresent = toClear;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = capturingThisFrame ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                             : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.srcAccessMask =
        capturingThisFrame ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(commands[slot], VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toPresent);

    vkEndCommandBuffer(commands[slot]);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired[slot];
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands[slot];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &presentReady[imageIndex];
    if (gears::SharedGpuQueueAccess().Submit(queue, 1, &submit, submitted[slot]) != VK_SUCCESS)
        return false;
    sources.Submitted(slot, std::move(drawnLease));

    if (capturingThisFrame)
    {
        // Wait for THIS frame's submit before reading the buffer. It stalls the
        // presenter for one frame, which is the right trade for a gated diagnostic
        // and is why it is not on by default.
        if (vkWaitForFences(device, 1, &submitted[slot], VK_TRUE, UINT64_MAX) == VK_SUCCESS)
        {
            sources.Release(slot);
            if (selfCheckThisFrame)
                CheckPresentedFrameLooksFinished(extent.width, extent.height, format);
            if (presentCaptureWanted > 0 && sequence >= presentCaptureAfter)
            {
                WritePresentedFrame(extent.width, extent.height, format);
                --presentCaptureWanted;
            }
        }
        else
        {
            lucent::warn("present",
                         "a presented-frame capture was requested but"
                         " the fence wait failed, so nothing was written -- do not read the"
                         " absence of a file as an empty frame");
            presentCaptureWanted = 0;
            gears::SharedGpuQueueAccess().WaitDeviceIdle(device);
            sources.ResetAfterDeviceIdle();
        }
    }

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &presentReady[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    r = gears::SharedGpuQueueAccess().Present(queue, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        gears::SharedGpuQueueAccess().WaitDeviceIdle(device);
        sources.ResetAfterDeviceIdle();
        return CreateSwapchain();
    }
    if (r != VK_SUCCESS)
    {
        lucent::warn("present", "vkQueuePresentKHR ({})", ResultName(r));
        gears::SharedGpuQueueAccess().WaitDeviceIdle(device);
        sources.ResetAfterDeviceIdle();
        return false;
    }
    return true;
}

void Presenter::PumpEvents()
{
    // Nothing to pump without a window, and SDL's video subsystem was never
    // started on that path.
    if (headlessSurface)
        return;
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {

        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        {
            // Closing the window must not kill the guest: the runtime's job is
            // to keep executing the title. Presenting stops, everything else
            // carries on exactly as in a headless run.
            lucent::info("present",
                         "window closed after {} presents;"
                         " continuing headless",
                         presentCount);
            running = false;
        }
    }
    // The presenter thread owns SDL, so it is the thread that reads the pad and
    // the keyboard. Everything else sees the published snapshot.
    gears::PollHostInput();
}

void Presenter::Thread()
{
    // SDL otherwise installs SIGINT/SIGTERM handlers that turn a kill into an
    // SDL_EVENT_QUIT. Measured: the runtime then ignored SIGTERM entirely and
    // had to be SIGKILLed, which breaks `timeout` in every measurement harness
    // here. The window layer must not take over the process's signal disposition.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    headlessSurface = lucent::config::flag("PRESENT_HEADLESS");

    // GAMEPAD as well as VIDEO: the presenter thread is the one that reads the
    // pad, because it is the thread that owns the SDL event queue.
    if (!headlessSurface && !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        lucent::warn("present", "SDL_Init(VIDEO|GAMEPAD) failed: {} -- running headless",
                     SDL_GetError());
        return;
    }
    if (!headlessSurface && !SDL_Vulkan_LoadLibrary(nullptr))
    {
        lucent::warn("present", "SDL_Vulkan_LoadLibrary failed: {} -- running headless",
                     SDL_GetError());
        return;
    }

    if (!headlessSurface)
        window = SDL_CreateWindow(gears::kHostProductName, kWindowWidth, kWindowHeight,
                                  SDL_WINDOW_VULKAN);
    if (!headlessSurface && window == nullptr)
    {
        lucent::warn("present", "SDL_CreateWindow failed: {} -- running headless", SDL_GetError());
        return;
    }

    if (!CreateInstanceAndDevice() || !CreateSwapchain())
    {
        lucent::warn("present", "no usable Vulkan presentation path -- running headless");
        return;
    }

    // GEARS_PRESENT_DUMP=N writes the next N presented frames. This is the only way
    // to see what actually reaches the window: the draw path's screenshots come from
    // its own readback and prove what it RENDERED, not what the presenter put on
    // screen. The distinction is about to matter -- the next change removes that
    // readback and blits the drawn image straight into the swapchain, and a blit can
    // go wrong in colour, orientation or extent while still "not crashing".
    presentCaptureWanted = uint64_t(lucent::config::number("PRESENT_DUMP", 0));
    // GEARS_PRESENT_DUMP_AT delays the capture. Without it the first presents are
    // captured, which are clear colours -- the draw path has not produced anything
    // by then -- and a flat frame is not evidence about the render path.
    presentCaptureAfter = uint64_t(lucent::config::number("PRESENT_DUMP_AT", 0));
    if (presentCaptureWanted != 0 && !canCapturePresented)
    {
        // Refuse loudly rather than writing nothing: an absent file would read as a
        // capture that came out empty.
        lucent::warn("present",
                     "{} presented frame(s) were requested but this"
                     " surface does not permit TRANSFER_SRC on its swapchain images, so NONE"
                     " can be captured. This is a refusal, not an empty frame.",
                     presentCaptureWanted);
        presentCaptureWanted = 0;
    }
    else if (presentCaptureWanted != 0)
    {
        lucent::info("present",
                     "will write the next {} presented frame(s) to"
                     " scratch/screenshots (override with GEARS_PRESENT_DUMP_DIR)",
                     presentCaptureWanted);
    }

    lucent::info("present", "window up; presenting the drawn guest frame on the"
                            " guest's VdSwap. Presents before the first drawn frame go out black.");
    running = true;
    lastReport = std::chrono::steady_clock::now();

    while (!shuttingDown.load())
    {
        uint32_t sequence = 0;
        {
            std::unique_lock<std::mutex> lock(mutex);
            // The 8 ms timeout exists only so the event queue is serviced while
            // the guest is between frames. It never presents: a present happens
            // solely because the command processor asked for one.
            request.wait_for(lock, std::chrono::milliseconds(8),
                             [this] { return pending || shuttingDown.load(); });
            if (!pending)
            {
                lock.unlock();
                PumpEvents();
                continue;
            }
            pending = false;
            sequence = pendingSequence;
        }

        PumpEvents();

        const auto start = std::chrono::steady_clock::now();
        if (running.load())
        {
            if (!PresentOne(sequence))
            {
                lucent::warn("present", "presentation failed; continuing headless");
                running = false;
            }
        }
        const auto finish = std::chrono::steady_clock::now();

        presentCount++;
        presentMicros +=
            uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count());
        if (presentCount % 300 == 0)
        {
            const double seconds = std::chrono::duration<double>(finish - lastReport).count();
            lastReport = finish;
            lucent::info("present",
                         "{} presents ({} new frames, {} repeats of the"
                         " previous one, {} black before the first frame),"
                         " last 300 in {:.2f}s ({:.2f} fps), mean present cost {:.2f} ms",
                         presentCount, freshFramesShown, repeatedFramesShown, blankPresents,
                         seconds, seconds > 0 ? 300.0 / seconds : 0.0,
                         double(presentMicros) / double(presentCount) / 1000.0);
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            serviced = true;
        }
        done.notify_all();
    }
}

} // namespace

namespace gears
{

bool PresenterStart()
{
    if (lucent::config::flag("NO_WINDOW"))
    {
        lucent::warn("present", "GEARS_NO_WINDOW set: no host window, no presentation");
        return false;
    }

    static std::thread thread([] { g_presenter.Thread(); });
    thread.detach();

    // Wait briefly for the thread to decide whether it has a display. This is
    // start-up only; it does not pace anything afterwards.
    for (int i = 0; i < 200 && !g_presenter.running.load(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return g_presenter.running.load();
}

void PresentFrame(uint32_t frontBuffer, uint32_t sequence)
{
    if (!g_presenter.running.load())
        return;

    lucent::debug("present", "present seq {} (guest front buffer {:#x})", sequence, frontBuffer);

    std::unique_lock<std::mutex> lock(g_presenter.mutex);
    g_presenter.pending = true;
    g_presenter.pendingSequence = sequence;
    g_presenter.serviced = false;
    g_presenter.request.notify_one();
    // Waiting here is deliberate: it keeps one present per guest frame with no
    // queue in between, so whatever presenting costs is visible in the guest's
    // own frame rate rather than absorbed silently. The timeout is a safety
    // net -- a wedged presenter must not wedge the command processor.
    if (!g_presenter.done.wait_for(lock, std::chrono::milliseconds(500), []
                                   { return g_presenter.serviced || !g_presenter.running.load(); }))
        lucent::warn("present", "present did not complete within 500 ms (seq {})", sequence);
}

} // namespace gears

#else // GEARS_HAVE_PRESENTER

namespace gears
{

bool PresenterStart()
{
    lucent::warn("present", "built without SDL3/Vulkan: no host window, command stream only");
    return false;
}

void PresentFrame(uint32_t, uint32_t)
{
}

} // namespace gears

#endif
