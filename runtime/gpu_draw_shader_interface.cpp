// Xenia-backed decoding for the plain shader-interface contract. Keeping this
// tiny operation outside gpu_draw_xlate.cpp prevents the translator monolith
// from absorbing another ownership concern.
#include "gpu_draw_xlate.h"

#ifdef GEARS_HAVE_GUEST_DRAW

#include "xenia/gpu/spirv_shader_translator.h"

namespace gears::draw
{

uint32_t ShaderInterpolatorMask(bool isVertex, uint64_t modification)
{
    const xe::gpu::SpirvShaderTranslator::Modification m(modification);
    return isVertex ? m.vertex.interpolator_mask : m.pixel.interpolator_mask;
}

} // namespace gears::draw

#endif // GEARS_HAVE_GUEST_DRAW
