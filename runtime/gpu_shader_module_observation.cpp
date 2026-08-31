#include "gpu_shader_module_observation.h"

namespace gears
{

void ObserveGpuShaderModulesForDraw(std::uint32_t packetGuestAddress, std::uint64_t vertexHash,
                                    std::uint64_t pixelHash,
                                    const std::map<std::uint64_t, GpuCapturedShader> &shaders)
{
    if (!RhiSemanticObservationEnabled() || packetGuestAddress == 0)
        return;

    RhiShaderPacketModuleEvidence evidence{.packetGuestAddress = packetGuestAddress};
    const auto append =
        [&](std::uint64_t hash, std::uint32_t type, std::vector<RhiShaderModuleEvidence> &out)
    {
        const auto shader = shaders.find(hash);
        if (shader == shaders.end() || shader->second.type != type || shader->second.ucode.empty())
            return;
        out.push_back({.guestAddress = shader->second.address,
                       .sizeBytes = static_cast<std::uint32_t>(shader->second.ucode.size()),
                       .hash = hash});
    };
    append(vertexHash, 0, evidence.vertexModules);
    append(pixelHash, 1, evidence.pixelModules);
    if (!evidence.vertexModules.empty() || !evidence.pixelModules.empty())
        ObserveRhiShaderPacketModuleEvidence({evidence});
}

} // namespace gears
