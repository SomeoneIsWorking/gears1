// Xenos->SPIR-V translation and system-constants derivation for the guest-draw
// backend. Compiled against Xenia's headers ONLY (no system Vulkan headers --
// they conflict with Xenia's bundled Vulkan-Headers). See gpu_draw_xlate.h.
#include "gpu_draw_xlate.h"

#include <lucent/log.h>

#ifdef GEARS_HAVE_GUEST_DRAW

#include <algorithm>
#include <bit>
#include <cstring>

#include "xenia/base/string_buffer.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/xenos.h"

namespace gears::draw
{
namespace
{

using namespace xe::gpu; // XE_GPU_REG_* register-index enumerators
using xe::gpu::RegisterFile;
using xe::gpu::SpirvShaderTranslator;
namespace draw_util = xe::gpu::draw_util;
namespace reg = xe::gpu::reg;
namespace xenos = xe::gpu::xenos;

bool TranslateOne(SpirvShaderTranslator& translator, xenos::ShaderType type,
                  const uint8_t* ucode, size_t size, uint64_t hash,
                  ShaderXlate& out)
{
    if (size == 0 || size % 4 != 0)
    {
        lucent::warn("draw", "microcode size {} not a dword multiple", size);
        return false;
    }
    xe::gpu::SpirvShader shader(type, hash,
        reinterpret_cast<const uint32_t*>(ucode), size / 4, std::endian::big);
    xe::StringBuffer disasm;
    shader.AnalyzeUcode(disasm);
    if (!shader.is_ucode_analyzed())
    {
        lucent::warn("draw", "ucode analyze failed for {:#018x}", hash);
        return false;
    }
    uint64_t modification =
        type == xenos::ShaderType::kVertex
            ? translator.GetDefaultVertexShaderModification(
                  shader.GetDynamicAddressableRegisterCount(0))
            : translator.GetDefaultPixelShaderModification(
                  shader.GetDynamicAddressableRegisterCount(0));
    xe::gpu::Shader::Translation* translation =
        shader.GetOrCreateTranslation(modification);
    if (!translator.TranslateAnalyzedShader(*translation) ||
        !translation->is_valid())
    {
        lucent::warn("draw", "translate failed for {:#018x}", hash);
        return false;
    }
    const std::vector<uint8_t>& spirv = translation->translated_binary();
    if (spirv.empty())
        return false;
    out.spirv = spirv;
    const auto& map = shader.constant_register_map();
    for (int i = 0; i < 4; ++i)
        out.floatBitmap[i] = map.float_bitmap[i];
    out.floatCount = map.float_count;
    out.ok = true;
    lucent::info("draw", "translated {} {:#018x}: {} bytes SPIR-V, {} float constants",
        type == xenos::ShaderType::kVertex ? "VS" : "PS", hash, spirv.size(),
        out.floatCount);
    return true;
}

}  // namespace

bool TranslateHotPair(const uint8_t* vsUcode, size_t vsSize, uint64_t vsHash,
                      const uint8_t* psUcode, size_t psSize, uint64_t psHash,
                      ShaderXlate& outVs, ShaderXlate& outPs)
{
    // Same configuration as tools/xenos_translate (the widest translator path,
    // non-FSI host render targets -- what the verified .spv were built for).
    SpirvShaderTranslator translator(
        SpirvShaderTranslator::Features(/*all=*/true),
        /*native_2x_msaa_with_attachments=*/true,
        /*native_2x_msaa_no_attachments=*/true,
        /*edram_fragment_shader_interlock=*/false);
    bool a = TranslateOne(translator, xenos::ShaderType::kVertex, vsUcode, vsSize,
                          vsHash, outVs);
    bool b = TranslateOne(translator, xenos::ShaderType::kPixel, psUcode, psSize,
                          psHash, outPs);
    return a && b;
}

bool TranslateShader(bool isVertex, const uint8_t* ucode, size_t size,
                     uint64_t hash, ShaderXlate& out)
{
    SpirvShaderTranslator translator(
        SpirvShaderTranslator::Features(/*all=*/true),
        /*native_2x_msaa_with_attachments=*/true,
        /*native_2x_msaa_no_attachments=*/true,
        /*edram_fragment_shader_interlock=*/false);
    return TranslateOne(translator,
        isVertex ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel,
        ucode, size, hash, out);
}

std::vector<uint8_t> DeriveSystemConstants(const uint32_t* registerFile)
{
    RegisterFile regs;
    std::memcpy(regs.values, registerFile,
        RegisterFile::kRegisterCount * sizeof(uint32_t));

    SpirvShaderTranslator::SystemConstants sc;
    std::memset(&sc, 0, sizeof(sc));

    const uint32_t draw_resolution_scale_x = 1;
    const uint32_t draw_resolution_scale_y = 1;

    auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
    auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
    auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
    auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
    auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);

    reg::RB_DEPTHCONTROL normalized_depth_control =
        draw_util::GetNormalizedDepthControl(regs);
    bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);

    draw_util::ViewportInfo viewport_info;
    draw_util::GetViewportInfoArgs gviargs{};
    const uint32_t host_max_viewport_dim = 16384;
    gviargs.Setup(draw_resolution_scale_x, draw_resolution_scale_y,
                  xe::divisors::MagicDiv(draw_resolution_scale_x),
                  xe::divisors::MagicDiv(draw_resolution_scale_y),
                  /*origin_bottom_left=*/false, host_max_viewport_dim,
                  host_max_viewport_dim, /*allow_reverse_z=*/true,
                  normalized_depth_control, /*convert_z_to_float24=*/false,
                  /*full_float24_in_0_to_1=*/false,
                  /*pixel_shader_writes_depth=*/false);
    gviargs.SetupRegisterValues(regs);
    draw_util::GetHostViewportInfo(&gviargs, viewport_info);

    uint32_t flags = 0;
    if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
    if (pa_cl_vte_cntl.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
    if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
    if (primitive_polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
    if (draw_util::IsPrimitiveLine(regs)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
    flags |= uint32_t(rb_surface_info.msaa_samples)
             << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
    if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8)
        flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
    xenos::CompareFunction alpha_test_function =
        rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func
                                          : xenos::CompareFunction::kAlways;
    flags |= uint32_t(alpha_test_function)
             << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        auto color_info =
            regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
        if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
            flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    }
    sc.flags = flags;

    sc.vertex_index_endian = xenos::Endian::kNone;
    sc.vertex_base_index = vgt_indx_offset;

    for (uint32_t i = 0; i < 3; ++i) {
        sc.ndc_scale[i] = viewport_info.ndc_scale[i];
        sc.ndc_offset[i] = viewport_info.ndc_offset[i];
    }

    if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
        auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
        auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
        sc.point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
        sc.point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
        sc.point_constant_diameter[0] = float(pa_su_point_size.width) * (2.0f / 16.0f);
        sc.point_constant_diameter[1] = float(pa_su_point_size.height) * (2.0f / 16.0f);
        sc.point_screen_diameter_to_ndc_radius[0] =
            float(draw_resolution_scale_x) / std::max(viewport_info.xy_extent[0], 1u);
        sc.point_screen_diameter_to_ndc_radius[1] =
            float(draw_resolution_scale_y) / std::max(viewport_info.xy_extent[1], 1u);
    }

    sc.alpha_test_reference = rb_alpha_ref;
    sc.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                           ? (rb_colorcontrol.value >> 24) | (1 << 8)
                           : 0;
    sc.zpd_fsi_counter_index = UINT32_MAX;

    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
        auto color_info =
            regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
        int32_t color_exp_bias = color_info.color_exp_bias;
        float color_exp_bias_scale;
        int32_t bits = int32_t(0x3F800000) + (color_exp_bias << 23);
        std::memcpy(&color_exp_bias_scale, &bits, sizeof(bits));
        sc.color_exp_bias[i] = color_exp_bias_scale;
    }

    std::vector<uint8_t> out(sizeof(sc));
    std::memcpy(out.data(), &sc, sizeof(sc));
    return out;
}

} // namespace gears::draw

#endif // GEARS_HAVE_GUEST_DRAW
