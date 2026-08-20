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

    // Resolve, reinterpretation, and depth-alias passes access storage images whose
    // host format varies with the guest surface. Declaring one format in a shader
    // and binding another produces undefined values, so those modules declare
    // Unknown and these two features carry them.
    bool storageImageWithoutFormat = false;

    // PA_CL_CLIP_CNTL.clip_disable says the guest wants no near/far clipping at
    // all, and the host way to say that is depthClampEnable on the rasterizer.
    // Without it a primitive whose Z is a hair outside [0,1] is CLIPPED AWAY
    // rather than clamped -- and a full-screen fill written at z = -3.7e-09 is
    // exactly that case, which is how two of this title's mask fills vanished
    // before rasterisation (catalog #91). Xenia gates the same state on the same
    // feature, so a device without it renders as we did before.
    bool depthClamp = false;

    // Guest sampler fetches carry anisotropy explicitly. The renderer used to
    // decode and cache that value, then omit it from VkSamplerCreateInfo, so a
    // declared 4x sampler silently became isotropic.
    bool samplerAnisotropy = false;
};

// Fills `enable` with what to ask for on this physical device, and `caps` with what
// that will grant. Safe to call without creating a device, which is how the adopting
// side learns its capabilities.
void SelectDeviceFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures &enable,
                          DeviceCapabilities &caps);

} // namespace gears
