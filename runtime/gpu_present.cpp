// Host graphics backend: window, Vulkan swapchain, present.
//
// SCOPE. This translation unit puts a window on screen and flips it once per
// guest frame. It does not draw the game: it clears the swapchain image to a
// colour derived from the guest's own VdSwap sequence number. That is the whole
// point of this milestone -- the content is deliberately synthetic so that a
// window which changes colour is unambiguous proof that the guest's frame loop
// is driving host presentation, and cannot be mistaken for the title rendering.
// No shader translation, no textures, no geometry, no EDRAM resolve.
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

#include <lucent/log.h>

#ifdef GEARS_HAVE_PRESENTER

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include "gpu_draw.h"
#include "input.h"

namespace
{

constexpr uint32_t kWindowWidth = 1280;
constexpr uint32_t kWindowHeight = 720;

const char* ResultName(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
    case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    default: return "VkResult";
    }
}

// The synthetic frame colour. Full-saturation hue sweep keyed on the guest's
// VdSwap sequence, so consecutive guest frames are visibly different and a
// stalled guest is visible as a frozen colour.
void FrameColour(uint32_t sequence, float rgb[3])
{
    const float hue = float(sequence % 180u) * (6.0f / 180.0f); // 0..6
    const int sector = int(hue) % 6;
    const float f = hue - std::floor(hue);
    const float q = 1.0f - f;
    switch (sector)
    {
    case 0: rgb[0] = 1; rgb[1] = f; rgb[2] = 0; break;
    case 1: rgb[0] = q; rgb[1] = 1; rgb[2] = 0; break;
    case 2: rgb[0] = 0; rgb[1] = 1; rgb[2] = f; break;
    case 3: rgb[0] = 0; rgb[1] = q; rgb[2] = 1; break;
    case 4: rgb[0] = f; rgb[1] = 0; rgb[2] = 1; break;
    default: rgb[0] = 1; rgb[1] = 0; rgb[2] = q; break;
    }
}

struct Presenter
{
    // --- host objects, touched only by the present thread -------------------
    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> images;
    // One semaphore per swapchain image: a present-wait semaphore stays in use
    // until the presentation engine is done with that image, so it cannot be
    // recycled per in-flight frame without a race.
    std::vector<VkSemaphore> presentReady;

    VkPhysicalDeviceMemoryProperties memProps{};

    // Staging buffer for uploading a real guest frame (from the guest-draw
    // backend) into the swapchain image instead of the synthetic clear.
    VkBuffer guestStaging = VK_NULL_HANDLE;
    VkDeviceMemory guestStagingMem = VK_NULL_HANDLE;
    void* guestStagingMapped = nullptr;
    VkDeviceSize guestStagingSize = 0;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    static constexpr uint32_t kInFlight = 2;
    VkCommandBuffer commands[kInFlight]{};
    VkSemaphore acquired[kInFlight]{};
    VkFence submitted[kInFlight]{};
    uint32_t frameSlot = 0;
    bool slotUsed[kInFlight]{};

    // --- handshake with the command processor -------------------------------
    std::mutex mutex;
    std::condition_variable request;
    std::condition_variable done;
    bool pending = false;
    bool serviced = false;
    uint32_t pendingSequence = 0;
    std::atomic<bool> running{false};
    std::atomic<bool> shuttingDown{false};

    // --- measurement --------------------------------------------------------
    uint64_t presentCount = 0;
    uint64_t presentMicros = 0;
    std::chrono::steady_clock::time_point lastReport;

    bool Start();
    void Stop();
    void Thread();

    bool CreateInstanceAndDevice();
    bool CreateSwapchain();
    void DestroySwapchain();
    bool EnsureGuestStaging(VkDeviceSize size);
    bool PresentOne(uint32_t sequence);
    void PumpEvents();
};

Presenter g_presenter;

bool Presenter::CreateInstanceAndDevice()
{
    uint32_t sdlExtensionCount = 0;
    const char* const* sdlExtensions =
        SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (sdlExtensions == nullptr)
    {
        lucent::warn("present", "SDL_Vulkan_GetInstanceExtensions: {}", SDL_GetError());
        return false;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gears1";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = sdlExtensionCount;
    instanceInfo.ppEnabledExtensionNames = sdlExtensions;

    VkResult r = vkCreateInstance(&instanceInfo, nullptr, &instance);
    if (r != VK_SUCCESS)
    {
        lucent::warn("present", "vkCreateInstance failed ({})", ResultName(r));
        instance = VK_NULL_HANDLE;
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
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

        for (uint32_t i = 0; i < familyCount; i++)
        {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
                continue;
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported);
            if (supported != VK_TRUE)
                continue;

            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            const int score =
                props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 :
                props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
            if (score > bestScore)
            {
                bestScore = score;
                physical = candidate;
                queueFamily = i;
            }
            break;
        }
    }

    if (physical == VK_NULL_HANDLE)
    {
        lucent::warn("present", "no Vulkan device can present to this surface"
            " ({} device(s) enumerated)", deviceCount);
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;

    r = vkCreateDevice(physical, &deviceInfo, nullptr, &device);
    if (r != VK_SUCCESS)
    {
        lucent::warn("present", "vkCreateDevice failed ({})", ResultName(r));
        device = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

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

    lucent::info("present", "Vulkan device \"{}\" (queue family {})",
        props.deviceName, queueFamily);
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

    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& candidate : formats)
    {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM &&
            candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = candidate;
            break;
        }
    }
    format = chosen.format;

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
    // TRANSFER_DST because the frame is produced with vkCmdClearColorImage:
    // no render pass, no pipeline, no shaders -- none of that is in scope here.
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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

    presentReady.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &presentReady[i]);
    }

    lucent::info("present", "swapchain {}x{}, {} images, {}",
        extent.width, extent.height, count,
        presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO (vsync-paced)");
    return true;
}

void Presenter::DestroySwapchain()
{
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

    if (slotUsed[slot])
        vkWaitForFences(device, 1, &submitted[slot], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
        acquired[slot], VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR)
    {
        // The window was resized between frames. Rebuild and drop this frame
        // rather than presenting into a stale surface; the guest's next VdSwap
        // brings the next one.
        vkDeviceWaitIdle(device);
        for (bool& used : slotUsed) used = false;
        return CreateSwapchain();
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
    {
        lucent::warn("present", "vkAcquireNextImageKHR ({})", ResultName(r));
        return false;
    }

    slotUsed[slot] = true;
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

    // If the guest-draw backend has produced a real frame, present THAT (copied
    // in through a staging buffer) instead of the synthetic colour. The guest
    // frame is R8G8B8A8; the swapchain image is B8G8R8A8, so swap R/B on the way
    // in. When there is no guest frame, fall back to the synthetic hue sweep so
    // the present path is still exercised.
    const std::vector<uint8_t>& guest = gears::GuestFramePixels();
    const uint32_t gw = gears::GuestFrameWidth();
    const uint32_t gh = gears::GuestFrameHeight();
    bool uploadedGuest = false;
    if (!guest.empty() && gw == extent.width && gh == extent.height &&
        EnsureGuestStaging(VkDeviceSize(guest.size())))
    {
        uint8_t* dst = static_cast<uint8_t*>(guestStagingMapped);
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
        float rgb[3];
        FrameColour(sequence, rgb);
        VkClearColorValue colour{};
        colour.float32[0] = rgb[0];
        colour.float32[1] = rgb[1];
        colour.float32[2] = rgb[2];
        colour.float32[3] = 1.0f;
        vkCmdClearColorImage(commands[slot], images[imageIndex],
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
    }

    VkImageMemoryBarrier toPresent = toClear;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(commands[slot], VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);

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
    if (vkQueueSubmit(queue, 1, &submit, submitted[slot]) != VK_SUCCESS)
        return false;

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &presentReady[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain;
    present.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(queue, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
    {
        vkDeviceWaitIdle(device);
        for (bool& used : slotUsed) used = false;
        return CreateSwapchain();
    }
    if (r != VK_SUCCESS)
    {
        lucent::warn("present", "vkQueuePresentKHR ({})", ResultName(r));
        return false;
    }
    return true;
}

void Presenter::PumpEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {

        if (event.type == SDL_EVENT_QUIT ||
            event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        {
            // Closing the window must not kill the guest: the runtime's job is
            // to keep executing the title. Presenting stops, everything else
            // carries on exactly as in a headless run.
            lucent::info("present", "window closed after {} presents;"
                " continuing headless", presentCount);
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

    // GAMEPAD as well as VIDEO: the presenter thread is the one that reads the
    // pad, because it is the thread that owns the SDL event queue.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        lucent::warn("present", "SDL_Init(VIDEO|GAMEPAD) failed: {} -- running headless",
            SDL_GetError());
        return;
    }
    if (!SDL_Vulkan_LoadLibrary(nullptr))
    {
        lucent::warn("present", "SDL_Vulkan_LoadLibrary failed: {} -- running headless",
            SDL_GetError());
        return;
    }

    window = SDL_CreateWindow("gears1 (synthetic present test)",
        int(kWindowWidth), int(kWindowHeight), SDL_WINDOW_VULKAN);
    if (window == nullptr)
    {
        lucent::warn("present", "SDL_CreateWindow failed: {} -- running headless",
            SDL_GetError());
        return;
    }

    if (!CreateInstanceAndDevice() || !CreateSwapchain())
    {
        lucent::warn("present", "no usable Vulkan presentation path -- running headless");
        return;
    }

    lucent::info("present", "window up; presenting on the guest's VdSwap."
        " The colour sweep is SYNTHETIC -- nothing of the game is drawn.");
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
        presentMicros += uint64_t(
            std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count());
        if (presentCount % 300 == 0)
        {
            const double seconds =
                std::chrono::duration<double>(finish - lastReport).count();
            lastReport = finish;
            lucent::info("present", "{} presents, last 300 in {:.2f}s ({:.2f} fps),"
                " mean present cost {:.2f} ms",
                presentCount, seconds, seconds > 0 ? 300.0 / seconds : 0.0,
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
    if (getenv("GEARS_NO_WINDOW") != nullptr)
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

    lucent::debug("present", "present seq {} (guest front buffer {:#x})",
        sequence, frontBuffer);

    std::unique_lock<std::mutex> lock(g_presenter.mutex);
    g_presenter.pending = true;
    g_presenter.pendingSequence = sequence;
    g_presenter.serviced = false;
    g_presenter.request.notify_one();
    // Waiting here is deliberate: it keeps one present per guest frame with no
    // queue in between, so whatever presenting costs is visible in the guest's
    // own frame rate rather than absorbed silently. The timeout is a safety
    // net -- a wedged presenter must not wedge the command processor.
    if (!g_presenter.done.wait_for(lock, std::chrono::milliseconds(500),
            [] { return g_presenter.serviced || !g_presenter.running.load(); }))
        lucent::warn("present", "present did not complete within 500 ms (seq {})", sequence);
}

} // namespace gears

#else // GEARS_HAVE_PRESENTER

namespace gears
{

bool PresenterStart()
{
    lucent::warn("present",
        "built without SDL3/Vulkan: no host window, command stream only");
    return false;
}

void PresentFrame(uint32_t, uint32_t) {}

} // namespace gears

#endif
