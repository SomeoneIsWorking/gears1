#include "shader_binding_capture.h"

#include "guest_memory.h"
#include "guest_state_memory.h"
#include "rhi_resource_identity.h"

#include <cstddef>

namespace gears::titles::gears1
{
namespace
{

[[nodiscard]] std::uint32_t ReadGuestBe32(std::uint32_t address)
{
    return GuestStateMemory{gears::Memory().Base()}.Read32(address);
}

} // namespace

RhiBindingStateEvidence CaptureShaderBinding(ShaderStage stage, std::uint32_t device,
                                             std::span<const RhiShaderModuleEvidence> shaderModules,
                                             bool shaderModulesPresent)
{
    if (device == 0)
        return {};

    const ShaderSetterSpec spec = ShaderSetterSpecFor(stage);
    RhiBindingStateEvidence state{
        .present = true,
        .observedObject = ReadGuestBe32(device + spec.deviceShaderOffset),
        .textureFetchStatePresent = true,
        .shaderModulesPresent = shaderModulesPresent,
        .shaderModules = {shaderModules.begin(), shaderModules.end()},
    };
    constexpr std::uint32_t kDeviceRegisterShadowOffset = 0x400;
    for (std::size_t slot = 0; slot < kRhiTextureSlotCount; ++slot)
    {
        for (std::size_t dword = 0; dword < kRhiTextureDescriptorDwords; ++dword)
        {
            state.textureFetchState[slot][dword] =
                ReadGuestBe32(device + kDeviceRegisterShadowOffset +
                              (slot * kRhiTextureDescriptorDwords + dword) * sizeof(std::uint32_t));
        }
    }
    state.identity = CaptureRhiResourceIdentity(state.observedObject);
    return state;
}

} // namespace gears::titles::gears1
