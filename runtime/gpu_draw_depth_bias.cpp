#include "gpu_draw_depth_bias.h"

#ifdef GEARS_HAVE_GUEST_DRAW

#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"

namespace gears::draw
{
namespace
{

const xe::gpu::RegisterFile& AsRegisterFile(const uint32_t* words)
{
    return *reinterpret_cast<const xe::gpu::RegisterFile*>(words);
}

} // namespace

DepthBias DeriveDepthBias(const uint32_t* registerFile,
                          bool primitivePolygonal)
{
    using namespace xe::gpu;

    const RegisterFile& regs = AsRegisterFile(registerFile);
    DepthBias bias;
    draw_util::GetPreferredFacePolygonOffset(
        regs, primitivePolygonal, bias.slopeFactor, bias.constantFactor);

    const auto depthInfo = regs.Get<reg::RB_DEPTH_INFO>();
    bias.constantFactor *=
        depthInfo.depth_format == xenos::DepthRenderTargetFormat::kD24S8
            ? draw_util::kD3D10PolygonOffsetFactorUnorm24
            : draw_util::kD3D10PolygonOffsetFactorFloat24;
    bias.slopeFactor *= xenos::kPolygonOffsetScaleSubpixelUnit;
    return bias;
}

} // namespace gears::draw

#endif
