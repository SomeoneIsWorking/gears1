#include "guest_memory.h"
#include "guest_stack_argument.h"
#include "import_stub.h"
#include "rhi_semantic_stream.h"
#include "rhi_index_buffer.h"

#include <cstdint>

namespace
{

[[nodiscard]] std::uint32_t ReadGuestBe32(std::uint32_t address)
{
    return __builtin_bswap32(*gears::Memory().Translate<std::uint32_t>(address));
}

[[nodiscard]] bool HasTransientData(gears::RhiSemanticDrawKind kind)
{
    return kind == gears::RhiSemanticDrawKind::TransientVertices ||
           kind == gears::RhiSemanticDrawKind::TransientVerticesAndIndices;
}

[[nodiscard]] gears::RhiDrawPacketEvidence CaptureLastDrawPacket(std::uint32_t device, bool staged,
                                                                 gears::RhiSemanticDrawKind kind)
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
        gears::RhiDrawPacketEvidence evidence{
            .present = true,
            .opcode = opcode,
            .primitiveType = initiator & 0x3F,
            .sourceSelect = (initiator >> 6) & 0x3,
            .elementCount = initiator >> 16,
        };
        if (kind == gears::RhiSemanticDrawKind::BoundIndices && evidence.sourceSelect == 0)
        {
            const std::uint32_t dmaBaseIndex = initiatorIndex + 1;
            const std::uint32_t dmaSizeIndex = initiatorIndex + 2;
            if (payloadDwords <= dmaSizeIndex)
                return evidence;

            const std::uint32_t strideBytes = ((initiator >> 11) & 1) != 0 ? 4u : 2u;
            const std::uint32_t dmaBase = ReadGuestBe32(headerAddress + (dmaBaseIndex + 1) * 4);
            const std::uint32_t dmaSize = ReadGuestBe32(headerAddress + (dmaSizeIndex + 1) * 4);
            evidence.indexDataPresent = true;
            evidence.indexData = {
                .guestAddress = dmaBase & ~(strideBytes - 1),
                .sizeBytes = (dmaSize & 0x00FFFFFFu) * 2,
            };
            evidence.indexStrideBytes = strideBytes;
            evidence.indexEndianSwap = dmaSize >> 30;
        }
        if (HasTransientData(kind))
        {
            constexpr std::uint32_t kVertexAddressOffset = 13080;
            constexpr std::uint32_t kIndexAddressOffset = 13084;
            constexpr std::uint32_t kVertexDwordCountOffset = 13088;
            constexpr std::uint32_t kIndexDwordCountOffset = 13092;
            evidence.transientDataPresent = true;
            evidence.vertexData = {
                .guestAddress = ReadGuestBe32(device + kVertexAddressOffset),
                .sizeBytes = ReadGuestBe32(device + kVertexDwordCountOffset) * 4,
            };
            if (kind == gears::RhiSemanticDrawKind::TransientVerticesAndIndices)
            {
                evidence.indexData = {
                    .guestAddress = ReadGuestBe32(device + kIndexAddressOffset),
                    .sizeBytes = ReadGuestBe32(device + kIndexDwordCountOffset) * 4,
                };
            }
        }
        return evidence;
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

[[nodiscard]] gears::RhiSemanticBufferView CaptureIndexBufferView(std::uint32_t object)
{
    constexpr std::uint32_t kCommonFlagsOffset = 0;
    constexpr std::uint32_t kGuestAddressOffset = 24;
    constexpr std::uint32_t kSizeBytesOffset = 28;
    return gears::gears1::DecodeIndexBufferView(ReadGuestBe32(object + kCommonFlagsOffset),
                                                ReadGuestBe32(object + kGuestAddressOffset),
                                                ReadGuestBe32(object + kSizeBytesOffset));
}

[[nodiscard]] gears::RhiBindingStateEvidence CaptureIndexBufferBinding(std::uint32_t device)
{
    constexpr std::uint32_t kIndexBufferObjectOffset = 0x2F84;
    if (device == 0)
        return {};
    gears::RhiBindingStateEvidence state{
        .present = true,
        .observedObject = ReadGuestBe32(device + kIndexBufferObjectOffset),
    };
    if (state.observedObject != 0)
    {
        state.bufferViewPresent = true;
        state.bufferView = CaptureIndexBufferView(state.observedObject);
    }
    return state;
}

[[nodiscard]] gears::RhiBindingStateEvidence CaptureColorRenderTargetBinding(std::uint32_t device,
                                                                             std::uint32_t slot)
{
    constexpr std::uint32_t kRenderTargetObjectTableOffset = 0x2F88;
    constexpr std::uint32_t kRenderTargetDescriptorOffset = 0x2804;
    if (device == 0)
        return {};
    const std::uint32_t descriptorIndex = slot == 0 ? 0 : slot + 1;
    return {
        .present = true,
        .observedObject =
            ReadGuestBe32(device + kRenderTargetObjectTableOffset + slot * sizeof(std::uint32_t)),
        .descriptor = {ReadGuestBe32(device + kRenderTargetDescriptorOffset +
                                     descriptorIndex * sizeof(std::uint32_t))},
        .descriptorDwords = 1,
    };
}

[[nodiscard]] gears::RhiBindingStateEvidence CaptureDepthStencilTargetBinding(std::uint32_t device)
{
    constexpr std::uint32_t kDepthStencilObjectOffset = 0x2F98;
    constexpr std::uint32_t kDepthDescriptorWord0Offset = 0x2808;
    constexpr std::uint32_t kDepthDescriptorWord1Offset = 0x28C0;
    if (device == 0)
        return {};
    return {
        .present = true,
        .observedObject = ReadGuestBe32(device + kDepthStencilObjectOffset),
        .descriptor = {ReadGuestBe32(device + kDepthDescriptorWord0Offset),
                       ReadGuestBe32(device + kDepthDescriptorWord1Offset)},
        .descriptorDwords = 2,
    };
}

void ObserveAfterSuper(const gears::RhiSemanticDraw &draw, std::uint32_t device, bool staged)
{
    gears::ObserveRhiSemanticDraw(draw, CaptureLastDrawPacket(device, staged, draw.kind));
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

extern "C" PPC_FUNC(__imp__sub_8222AFD8);
PPC_FUNC(sub_8222AFD8)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_8222AFD8(ctx, base);
        return;
    }
    const std::uint32_t device = ctx.r3.u32;
    gears::RhiSemanticBinding binding{
        .kind = gears::RhiSemanticBindingKind::IndexBuffer,
        .object = ctx.r4.u32,
    };
    if (binding.object != 0)
    {
        binding.bufferViewPresent = true;
        binding.bufferView = CaptureIndexBufferView(binding.object);
    }
    __imp__sub_8222AFD8(ctx, base);
    gears::ObserveRhiSemanticBinding(binding, CaptureIndexBufferBinding(device));
}

extern "C" PPC_FUNC(__imp__sub_8222B068);
PPC_FUNC(sub_8222B068)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_8222B068(ctx, base);
        return;
    }
    const std::uint32_t device = ctx.r3.u32;
    const std::uint32_t slot = ctx.r4.u32;
    gears::RhiSemanticBinding binding{
        .kind = gears::RhiSemanticBindingKind::ColorRenderTarget,
        .slot = slot,
        .object = ctx.r5.u32,
    };
    __imp__sub_8222B068(ctx, base);
    if (binding.object != 0)
    {
        constexpr std::uint32_t kColorDescriptorWordOffset = 0x1C;
        binding.descriptor = {ReadGuestBe32(binding.object + kColorDescriptorWordOffset)};
        binding.descriptorDwords = 1;
    }
    gears::ObserveRhiSemanticBinding(binding, CaptureColorRenderTargetBinding(device, slot));
}

extern "C" PPC_FUNC(__imp__sub_8222B398);
PPC_FUNC(sub_8222B398)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_8222B398(ctx, base);
        return;
    }
    const std::uint32_t device = ctx.r3.u32;
    gears::RhiSemanticBinding binding{
        .kind = gears::RhiSemanticBindingKind::DepthStencilTarget,
        .object = ctx.r4.u32,
    };
    __imp__sub_8222B398(ctx, base);
    if (binding.object != 0)
    {
        constexpr std::uint32_t kDepthDescriptorWord0Offset = 0x1C;
        constexpr std::uint32_t kDepthDescriptorWord1Offset = 0x20;
        binding.descriptor = {ReadGuestBe32(binding.object + kDepthDescriptorWord0Offset),
                              ReadGuestBe32(binding.object + kDepthDescriptorWord1Offset)};
        binding.descriptorDwords = 2;
    }
    gears::ObserveRhiSemanticBinding(binding, CaptureDepthStencilTargetBinding(device));
}

extern "C" PPC_FUNC(__imp__sub_8222CFF8);
PPC_FUNC(sub_8222CFF8)
{
    if (!gears::RhiSemanticObservationEnabled())
    {
        __imp__sub_8222CFF8(ctx, base);
        return;
    }
    gears::RhiSemanticDraw draw{
        .kind = gears::RhiSemanticDrawKind::TransientVertices,
        .primitiveType = ctx.r4.u32,
        .elementCount = ctx.r5.u32,
        .vertexStrideBytes = ctx.r6.u32,
    };
    const std::uint32_t device = ctx.r3.u32;
    __imp__sub_8222CFF8(ctx, base);
    draw.vertexData = {
        .guestAddress = ctx.r3.u32,
        .sizeBytes = (draw.elementCount * draw.vertexStrideBytes) & ~std::uint32_t{3},
    };
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
    gears::RhiSemanticDraw draw{
        .kind = gears::RhiSemanticDrawKind::TransientVerticesAndIndices,
        .primitiveType = ctx.r4.u32,
        .elementCount = ctx.r7.u32,
        .baseVertex = ctx.r5.u32,
        .vertexStrideBytes = ctx.r9.u32,
        .indexFormatFlags = ctx.r8.u32,
    };
    const std::uint32_t device = ctx.r3.u32;
    const std::uint32_t vertexCount = ctx.r6.u32;
    const std::uint32_t indexDataOutput = ctx.r10.u32;
    const std::uint32_t vertexDataOutput = gears::GuestStackArgument32(base, ctx.r1.u32, 8);
    __imp__sub_8222D4F8(ctx, base);
    if (ctx.r3.u32 == 0)
    {
        draw.vertexData = {
            .guestAddress = ReadGuestBe32(vertexDataOutput),
            .sizeBytes = (vertexCount * draw.vertexStrideBytes) & ~std::uint32_t{3},
        };
        const std::uint32_t indexDwords =
            (draw.indexFormatFlags & 4) != 0 ? draw.elementCount : (draw.elementCount + 1) / 2;
        draw.indexData = {
            .guestAddress = ReadGuestBe32(indexDataOutput),
            .sizeBytes = indexDwords * 4,
        };
    }
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
    gears::RhiSemanticDraw draw{
        .kind = gears::RhiSemanticDrawKind::BoundIndices,
        .primitiveType = ctx.r4.u32,
        .elementCount = ctx.r7.u32,
        .baseVertex = ctx.r5.u32,
        .startIndex = ctx.r6.u32,
    };
    const std::uint32_t device = ctx.r3.u32;
    const gears::RhiBindingStateEvidence indexBinding = CaptureIndexBufferBinding(device);
    if (indexBinding.bufferViewPresent)
    {
        draw.indexBufferViewPresent = true;
        draw.indexBuffer = indexBinding.bufferView;
        const auto slice =
            gears::gears1::IndexBufferSlice(draw.indexBuffer, draw.startIndex, draw.elementCount);
        if (slice.has_value())
            draw.indexData = *slice;
    }
    __imp__sub_8222DE50(ctx, base);
    ObserveAfterSuper(draw, device, false);
}
