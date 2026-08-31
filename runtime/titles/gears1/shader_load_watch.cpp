#include "gpu_shader_load_watch.h"
#include "guest_state_memory.h"
#include "import_stub.h"
#include "shader_setter_state.h"

#include <array>
#include <cstdint>

extern "C" PPC_FUNC(__imp__sub_828D2930);
extern "C" PPC_FUNC(__imp__sub_8254CFA0);
extern "C" PPC_FUNC(__imp__sub_8254F2B0);

PPC_FUNC(sub_828D2930)
{
    if (!gears::ShaderLoadPacketWatchEnabled()) [[likely]]
    {
        __imp__sub_828D2930(ctx, base);
        return;
    }
    gears::ObserveShaderLoadPacketCopy(ctx.r3.u32, ctx.r5.u32, std::uint32_t(ctx.lr));
    __imp__sub_828D2930(ctx, base);
}

PPC_FUNC(sub_8254F2B0)
{
    if (!gears::ShaderLoadPacketWatchEnabled()) [[likely]]
    {
        __imp__sub_8254F2B0(ctx, base);
        return;
    }
    const std::uint32_t caller = std::uint32_t(ctx.lr);
    const std::uint32_t stackPointer = ctx.r1.u32;
    __imp__sub_8254F2B0(ctx, base);
    gears::ReportShaderLoadPacketProducerReturn(caller, stackPointer);
}

PPC_FUNC(sub_8254CFA0)
{
    if (!gears::ShaderLoadPacketWatchEnabled()) [[likely]]
    {
        __imp__sub_8254CFA0(ctx, base);
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
    const std::array<std::uint32_t, 2> shaderObjectsBefore = {
        memory.Read32(arguments[1] + pixel.deviceShaderOffset),
        memory.Read32(arguments[1] + vertex.deviceShaderOffset),
    };
    __imp__sub_8254CFA0(ctx, base);
    const std::array<std::uint32_t, 2> shaderObjectsAfter = {
        memory.Read32(arguments[1] + pixel.deviceShaderOffset),
        memory.Read32(arguments[1] + vertex.deviceShaderOffset),
    };
    gears::ReportShaderLoadPacketProducerParent(copySequence, caller, arguments,
                                                shaderObjectsBefore, shaderObjectsAfter);
}
