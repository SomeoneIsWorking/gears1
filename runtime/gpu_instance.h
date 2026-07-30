// One VkInstance for the whole runtime.
//
// gpu_draw.cpp and gpu_present.cpp each created their own instance AND their own
// device. Two devices is the expensive half -- the drawn image lives on one and the
// swapchain on the other, so every frame is read back to host memory and uploaded
// again through a staging buffer to reach the window -- but the instance has to be
// shared FIRST, because a VkSurfaceKHR created from one instance cannot be used
// with a device from another. This is that step.
//
// The awkward part is that the two callers need different instance extensions: the
// present path needs whatever SDL requires to make a surface, and the draw path
// wants debug utils when validation is on. Whoever initialises first creates the
// instance, so the set it asks for has to satisfy the other as well.
//
// A LATER CALLER ASKING FOR SOMETHING THE INSTANCE LACKS IS REPORTED, NOT IGNORED.
// Silently handing back an instance without the surface extensions would fail at
// SDL_Vulkan_CreateSurface with an error that names nothing, one layer removed from
// the cause.
#pragma once

#include <string>
#include <vector>

namespace gears
{

// Does an instance created with `created` satisfy a caller that needs `required`?
// Separated out because it is the only part of this that can be tested without a
// Vulkan driver, and it is where a silent mismatch would hide.
bool InstanceExtensionsSatisfy(const std::vector<std::string>& created,
                               const std::vector<std::string>& required,
                               std::vector<std::string>& missing);

// Merges two extension lists, preserving order and dropping duplicates. Vulkan
// rejects a duplicated extension name, and SDL's list overlaps ours.
std::vector<std::string> MergeInstanceExtensions(
    const std::vector<std::string>& first,
    const std::vector<std::string>& second);

} // namespace gears
