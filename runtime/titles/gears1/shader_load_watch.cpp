#include "gpu_shader_load_watch.h"
#include "import_stub.h"

#include <cstdint>

extern "C" PPC_FUNC(__imp__sub_828D2930);
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
