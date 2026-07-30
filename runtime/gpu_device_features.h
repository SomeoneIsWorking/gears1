// The device features this runtime's renderer needs, in one place.
//
// gpu_draw.cpp chose them inline while creating its own device, and gpu_present.cpp
// created its device with NO features at all. That is fine while there are two
// devices and fatal the moment there is one: whichever side creates the shared
// device must enable everything the other will use, and adopting a device created
// without these would be undefined behaviour showing up as validation errors and
// wrong pixels rather than a clean failure.
//
// Each feature is requested only if the physical device HAS it, and the
// corresponding capability flag says whether it was granted -- the renderer has real
// fallbacks for all three, so a device lacking one is a reduced renderer and not a
// dead one.
#pragma once

#include <vulkan/vulkan.h>

namespace gears
{

// What the renderer got, so it can take its fallbacks knowingly. Derived from the
// same physical-device query on both sides, so an adopter reaches the same answers
// as the creator without being told.
struct DeviceCapabilities
{
    // Pipeline statistics let a draw report how far it actually got -- vertices in,
    // primitives after clipping, fragment invocations. Without it, "this draw added
    // no pixels" cannot be told apart from "this draw was clipped away" or "this
    // draw shaded black".
    bool pipelineStatistics = false;

    // A rectangle list gives three vertices and the hardware infers the fourth.
    // Deriving it needs the shaded vertices, so it happens in a geometry shader.
    bool geometryShader = false;

    // The resolve compute pass reads and writes storage images whose format it does
    // not know at build time: an EDRAM surface may be 8888, 7e3 carried as
    // half-float, or two-channel float, and the destination is whatever the guest
    // asked for. Declaring a format in the shader and binding another silently
    // returns garbage, so the shader declares Unknown and these two features carry
    // it.
    bool storageImageWithoutFormat = false;
};

// Fills `enable` with what to ask for on this physical device, and `caps` with what
// that will grant. Safe to call without creating a device, which is how the adopting
// side learns its capabilities.
void SelectDeviceFeatures(VkPhysicalDevice physical,
                          VkPhysicalDeviceFeatures& enable,
                          DeviceCapabilities& caps);

} // namespace gears
