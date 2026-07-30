// Choosing the Vulkan queue family, for a runtime that needs ONE device to both
// draw and present.
//
// gpu_draw.cpp and gpu_present.cpp each create their own VkInstance and VkDevice,
// and each picks a queue family with its own inline loop that checks only its own
// half of the requirement. The cost is not the duplication: because the drawn image
// lives on one device and the swapchain on the other, every rendered frame is read
// back to host memory and uploaded again through a staging buffer to reach the
// window. One device would need neither step.
//
// Unifying them makes the choice harder rather than easier -- a single family must
// satisfy graphics AND presentation to the window's surface -- so the policy is
// factored out here where it can be tested without a GPU.
#pragma once

#include <cstdint>
#include <vector>

namespace gears
{

// What one family can do, reduced to what the decision actually depends on.
struct QueueFamily
{
    bool graphics = false;  // VK_QUEUE_GRAPHICS_BIT
    bool present = false;   // vkGetPhysicalDeviceSurfaceSupportKHR for OUR surface
    uint32_t count = 0;     // queueCount; a family with none is unusable
};

// No family qualifies. This is a real answer and the caller must fail on it
// LOUDLY: returning family 0 and hoping is how a port ends up with a device that
// cannot present, whose only symptom is a window that stays blank long after the
// mistake.
constexpr uint32_t kNoQueueFamily = 0xFFFFFFFFu;

// Picks the lowest-indexed family that can do everything required. `needPresent`
// is false when running headless, where there is no surface to consult.
//
// The lowest index rather than any qualifying one, so the choice is reproducible:
// a selector that picked arbitrarily would let a GPU-dependent bug reproduce on one
// machine and not another.
uint32_t ChooseQueueFamily(const std::vector<QueueFamily>& families,
                           bool needPresent);

} // namespace gears
