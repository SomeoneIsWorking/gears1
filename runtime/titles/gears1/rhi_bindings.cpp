#include "guest_memory.h"
#include "import_stub.h"
#include "rhi_semantic_stream.h"

#include <cstdint>

namespace
{

[[nodiscard]] std::uint32_t ReadGuestBe32(std::uint32_t address)
{
    return __builtin_bswap32(*gears::Memory().Translate<std::uint32_t>(address));
}

[[nodiscard]] gears::RhiDrawPacketEvidence CaptureLastDrawPacket(std::uint32_t device, bool staged)
{
    constexpr std::uint32_t kCommandWritePointerOffset = 0x28;
    constexpr std::uint32_t kStagedCommandEndOffset = 0x3314;
    constexpr std::uint32_t kDrawIndx = 0x22;
    constexpr std::uint32_t kDrawIndx2 = 0x36;
    constexpr std::uint32_t kSearchDwords = 32;

    const std::uint32_t end =
        ReadGuestBe32(device + (staged ? kStagedCommandEndOffset : kCommandWritePointerOffset));
    for (std::uint32_t distance = 0; distance < kSearchDwords && end >= distance * 4; ++distance)
    {
        const std::uint32_t headerAddress = end - distance * 4;
        const std::uint32_t header = ReadGuestBe32(headerAddress);
        if ((header >> 30) != 3)
            continue;
        const std::uint32_t opcode = (header >> 8) & 0x7F;
        if (opcode != kDrawIndx && opcode != kDrawIndx2)
            continue;

        const std::uint32_t payloadDwords = ((header >> 16) & 0x3FFF) + 1;
        const std::uint32_t initiatorIndex = opcode == kDrawIndx ? 1u : 0u;
        if (payloadDwords <= initiatorIndex || headerAddress + payloadDwords * 4 > end)
            continue;
        const std::uint32_t initiator = ReadGuestBe32(headerAddress + (initiatorIndex + 1) * 4);
        return {.present = true,
                .opcode = opcode,
                .primitiveType = initiator & 0x3F,
                .sourceSelect = (initiator >> 6) & 0x3,
                .elementCount = initiator >> 16};
    }
    return {};
}

[[nodiscard]] gears::RhiBindingStateEvidence CaptureTextureBinding(std::uint32_t device,
                                                                   std::uint32_t slot)
{
    constexpr std::uint32_t kTextureFetchShadowOffset = 1024;
    constexpr std::uint32_t kTextureFetchDwords = 6;
    constexpr std::uint32_t kTextureObjectTableIndex = 3068;

    if (device == 0)
        return {};

    gears::RhiBindingStateEvidence state{
        .present = true,
        .observedObject = ReadGuestBe32(device + (slot + kTextureObjectTableIndex) * 4),
        .descriptorDwords = kTextureFetchDwords,
    };
    const std::uint32_t shadow =
        device + slot * kTextureFetchDwords * 4 + kTextureFetchShadowOffset;
    for (std::uint32_t index = 0; index < kTextureFetchDwords; ++index)
        state.descriptor[index] = ReadGuestBe32(shadow + index * 4);
    return state;
}

[[nodiscard]] gears::RhiBindingStateEvidence CaptureShaderBinding(std::uint32_t device,
                                                                  std::uint32_t fieldOffset)
{
    if (device == 0)
        return {};
    return {
        .present = true,
        .observedObject = ReadGuestBe32(device + fieldOffset),
    };
}

void ObserveAfterSuper(const gears::RhiSemanticDraw &draw, std::uint32_t device, bool staged)
{
    gears::ObserveRhiSemanticDraw(draw, CaptureLastDrawPacket(device, staged));
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_82220858);
PPC_FUNC(sub_82220858)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_82220858(ctx, base);
        return;
    }
    const std::uint32_t device = ctx.r3.u32;
    const std::uint32_t slot = ctx.r4.u32;
    const gears::RhiSemanticBinding binding{
        .kind = gears::RhiSemanticBindingKind::Texture,
        .slot = slot,
        .object = ctx.r5.u32,
    };
    __imp__sub_82220858(ctx, base);
    gears::ObserveRhiSemanticBinding(binding, CaptureTextureBinding(device, slot));
}

extern "C" PPC_FUNC(__imp__sub_82222808);
PPC_FUNC(sub_82222808)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_82222808(ctx, base);
        return;
    }
    constexpr std::uint32_t kPixelShaderFieldOffset = 0x3080;
    const std::uint32_t device = ctx.r3.u32;
    const gears::RhiSemanticBinding binding{
        .kind = gears::RhiSemanticBindingKind::PixelShader,
        .object = ctx.r4.u32,
    };
    __imp__sub_82222808(ctx, base);
    gears::ObserveRhiSemanticBinding(binding,
                                     CaptureShaderBinding(device, kPixelShaderFieldOffset));
}

extern "C" PPC_FUNC(__imp__sub_82222B98);
PPC_FUNC(sub_82222B98)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_82222B98(ctx, base);
        return;
    }
    constexpr std::uint32_t kVertexShaderFieldOffset = 0x3084;
    const std::uint32_t device = ctx.r3.u32;
    const gears::RhiSemanticBinding binding{
        .kind = gears::RhiSemanticBindingKind::VertexShader,
        .object = ctx.r4.u32,
    };
    __imp__sub_82222B98(ctx, base);
    gears::ObserveRhiSemanticBinding(binding,
                                     CaptureShaderBinding(device, kVertexShaderFieldOffset));
}

extern "C" PPC_FUNC(__imp__sub_8222CFF8);
PPC_FUNC(sub_8222CFF8)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_8222CFF8(ctx, base);
        return;
    }
    const gears::RhiSemanticDraw draw{
        .kind = gears::RhiSemanticDrawKind::TransientVertices,
        .primitiveType = ctx.r4.u32,
        .elementCount = ctx.r5.u32,
        .vertexStrideBytes = ctx.r6.u32,
    };
    const std::uint32_t device = ctx.r3.u32;
    __imp__sub_8222CFF8(ctx, base);
    ObserveAfterSuper(draw, device, true);
}

extern "C" PPC_FUNC(__imp__sub_8222D4F8);
PPC_FUNC(sub_8222D4F8)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_8222D4F8(ctx, base);
        return;
    }
    const gears::RhiSemanticDraw draw{
        .kind = gears::RhiSemanticDrawKind::TransientVerticesAndIndices,
        .primitiveType = ctx.r4.u32,
        .elementCount = ctx.r7.u32,
        .baseVertex = ctx.r5.u32,
        .vertexStrideBytes = ctx.r9.u32,
        .indexFormatFlags = ctx.r8.u32,
    };
    const std::uint32_t device = ctx.r3.u32;
    __imp__sub_8222D4F8(ctx, base);
    ObserveAfterSuper(draw, device, true);
}

extern "C" PPC_FUNC(__imp__sub_8222DA48);
PPC_FUNC(sub_8222DA48)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_8222DA48(ctx, base);
        return;
    }
    const gears::RhiSemanticDraw draw{
        .kind = gears::RhiSemanticDrawKind::BoundVertices,
        .primitiveType = ctx.r4.u32,
        .elementCount = ctx.r6.u32,
        .baseVertex = ctx.r5.u32,
    };
    const std::uint32_t device = ctx.r3.u32;
    __imp__sub_8222DA48(ctx, base);
    ObserveAfterSuper(draw, device, false);
}

extern "C" PPC_FUNC(__imp__sub_8222DE50);
PPC_FUNC(sub_8222DE50)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_8222DE50(ctx, base);
        return;
    }
    const gears::RhiSemanticDraw draw{
        .kind = gears::RhiSemanticDrawKind::BoundIndices,
        .primitiveType = ctx.r4.u32,
        .elementCount = ctx.r7.u32,
        .baseVertex = ctx.r5.u32,
        .startIndex = ctx.r6.u32,
    };
    const std::uint32_t device = ctx.r3.u32;
    __imp__sub_8222DE50(ctx, base);
    ObserveAfterSuper(draw, device, false);
}
