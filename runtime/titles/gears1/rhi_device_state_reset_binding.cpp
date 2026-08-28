#include "guest_state_memory.h"
#include "import_stub.h"
#include "rhi_semantic_stream.h"
#include "rhi_vertex_buffer.h"

#include <cstdint>

namespace
{

constexpr std::uint32_t kD3dDeviceGlobal = 0x82BECBA0;

[[nodiscard]] gears::RhiVertexStreamResetEvidence
CaptureVertexStreamResetState(std::uint8_t *base, std::uint32_t device)
{
    if (device == 0)
        return {};

    const gears::titles::gears1::GuestStateMemory memory(base);
    gears::RhiVertexStreamResetEvidence state{.present = true};
    for (std::uint32_t slot = 0; slot < gears::gears1::kVertexStreamSlotCount; ++slot)
    {
        const std::uint32_t object = memory.Read32(
            device + gears::gears1::kVertexStreamObjectTableOffset + slot * sizeof(std::uint32_t));
        if (object != 0)
            state.activeStreams.push_back({.slot = slot, .object = object});
    }
    return state;
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_82487510);
PPC_FUNC(sub_82487510)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_82487510(ctx, base);
        return;
    }

    const gears::titles::gears1::GuestStateMemory memory(base);
    const std::uint32_t device = memory.Read32(kD3dDeviceGlobal);
    __imp__sub_82487510(ctx, base);
    gears::ObserveRhiSemanticVertexStreamReset(
        {.firstSlot = 0, .slotCount = gears::gears1::kVertexStreamSlotCount},
        CaptureVertexStreamResetState(base, device));
}
