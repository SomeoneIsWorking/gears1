#include "gpu_shader_load_watch.h"
#include "guest_probe_state.h"
#include "guest_state_memory.h"
#include "import_stub.h"
#include "shader_setter_state.h"
#include "rhi_shader_object_watch.h"

#include <array>
#include <cstdint>
#include <string_view>

#include <lucent/log.h>

extern "C" PPC_FUNC(__imp__sub_828D2930);
extern "C" PPC_FUNC(__imp__sub_82327E00);
extern "C" PPC_FUNC(__imp__sub_8254CFA0);
extern "C" PPC_FUNC(__imp__sub_8254F2B0);

namespace
{

using ShaderLoadFields = std::array<std::uint32_t, 5>;

constexpr ShaderLoadFields kSerializerCommandFieldOffsets = {0x4, 0x10, 0x1C, 0x28, 0x40};
constexpr ShaderLoadFields kOuterCallbackFieldOffsets = {0x10, 0x20, 0x60, 0x64, 0x68};

ShaderLoadFields ReadShaderLoadFields(const gears::titles::gears1::GuestStateMemory &memory,
                                      std::uint32_t object, const ShaderLoadFields &offsets)
{
    ShaderLoadFields fields{};
    for (std::size_t index = 0; index < offsets.size(); ++index)
        fields[index] = memory.Read32(object + offsets[index]);
    return fields;
}

void ReportShaderLoadFields(std::string_view label, const ShaderLoadFields &before,
                            const ShaderLoadFields &after)
{
    lucent::info("hle",
                 "shader-load {} fields: "
                 "{:#x}/{:#x} {:#x}/{:#x} {:#x}/{:#x} {:#x}/{:#x} {:#x}/{:#x}",
                 label, before[0], after[0], before[1], after[1], before[2], after[2], before[3],
                 after[3], before[4], after[4]);
}

} // namespace

PPC_FUNC(sub_828D2930)
{
    if (!(gears::ShaderLoadPacketWatchEnabled() ||
          gears::titles::gears1::RhiPixelShaderObjectWatchEnabled())) [[likely]]
    {
        __imp__sub_828D2930(ctx, base);
        return;
    }
    gears::ObserveShaderLoadPacketCopy(ctx.r3.u32, ctx.r5.u32, std::uint32_t(ctx.lr));
    __imp__sub_828D2930(ctx, base);
}

PPC_FUNC(sub_8254F2B0)
{
    if (!(gears::ShaderLoadPacketWatchEnabled() ||
          gears::titles::gears1::RhiPixelShaderObjectWatchEnabled())) [[likely]]
    {
        __imp__sub_8254F2B0(ctx, base);
        return;
    }
    const std::uint32_t caller = std::uint32_t(ctx.lr);
    const std::uint32_t stackPointer = ctx.r1.u32;
    __imp__sub_8254F2B0(ctx, base);
    gears::ReportShaderLoadPacketProducerReturn(caller, stackPointer);
}

PPC_FUNC(sub_82327E00)
{
    if (!gears::ShaderLoadPacketWatchEnabled()) [[likely]]
    {
        __imp__sub_82327E00(ctx, base);
        return;
    }
    const std::uint64_t copySequence = gears::ShaderLoadPacketCopySequence();
    const gears::titles::gears1::GuestStateMemory memory(base);
    // The retained callback reads these fields to construct the serializer's
    // register arguments. They remain diagnostic evidence, not semantic state.
    const std::uint32_t callbackObject = ctx.r3.u32;
    const std::uint32_t callbackVtable = memory.Read32(callbackObject);
    const auto fieldsBefore =
        ReadShaderLoadFields(memory, callbackObject, kOuterCallbackFieldOffsets);
    __imp__sub_82327E00(ctx, base);
    if (copySequence != gears::ShaderLoadPacketCopySequence())
    {
        lucent::info("hle", "shader-load outer callback vtable {:#x}", callbackVtable);
        gears::titles::gears1::ReportRenderRingReservationForObject(callbackObject);
        ReportShaderLoadFields("outer callback +10/+20/+60/+64/+68", fieldsBefore, fieldsBefore);
    }
}

PPC_FUNC(sub_8254CFA0)
{
    const bool shaderPacketWatch = gears::ShaderLoadPacketWatchEnabled();
    const bool pixelObjectWatch = gears::titles::gears1::RhiPixelShaderObjectWatchEnabled();
    if (!(shaderPacketWatch || pixelObjectWatch)) [[likely]]
    {
        __imp__sub_8254CFA0(ctx, base);
        return;
    }
    if (!shaderPacketWatch)
    {
        __imp__sub_8254CFA0(ctx, base);
        gears::titles::gears1::ReportRhiPixelShaderObjectWriteWatch();
        return;
    }
    const std::uint64_t copySequence = gears::ShaderLoadPacketCopySequence();
    const std::uint32_t caller = std::uint32_t(ctx.lr);
    const std::array<std::uint32_t, 6> arguments = {
        ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
    };
    const gears::titles::gears1::GuestStateMemory memory(base);
    const auto pixel =
        gears::titles::gears1::ShaderSetterSpecFor(gears::titles::gears1::ShaderStage::Pixel);
    const auto vertex =
        gears::titles::gears1::ShaderSetterSpecFor(gears::titles::gears1::ShaderStage::Vertex);
    // These are the only callback-object fields the retained entry reads before
    // deciding whether it has work. They are observation-only evidence, never
    // a semantic shader binding.
    const auto commandFieldsBefore =
        ReadShaderLoadFields(memory, arguments[0], kSerializerCommandFieldOffsets);
    const std::array<std::uint32_t, 2> shaderObjectsBefore = {
        memory.Read32(arguments[1] + pixel.deviceShaderOffset),
        memory.Read32(arguments[1] + vertex.deviceShaderOffset),
    };
    __imp__sub_8254CFA0(ctx, base);
    const auto commandFieldsAfter =
        ReadShaderLoadFields(memory, arguments[0], kSerializerCommandFieldOffsets);
    const std::array<std::uint32_t, 2> shaderObjectsAfter = {
        memory.Read32(arguments[1] + pixel.deviceShaderOffset),
        memory.Read32(arguments[1] + vertex.deviceShaderOffset),
    };
    const bool selectedProducer = copySequence != gears::ShaderLoadPacketCopySequence();
    gears::ReportShaderLoadPacketProducerParent(copySequence, caller, arguments,
                                                shaderObjectsBefore, shaderObjectsAfter);
    if (selectedProducer)
        ReportShaderLoadFields("serializer command +4/+10/+1c/+28/+40", commandFieldsBefore,
                               commandFieldsAfter);
    gears::titles::gears1::ReportRhiPixelShaderObjectWriteWatch();
}
