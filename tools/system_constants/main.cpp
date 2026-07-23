// Offline driver: Xenos register state -> xe_uniform_system_constants UBO.
//
// This is a measurement/verification tool, not part of the runtime. It ports
// the per-draw derivation of Xenia's `SpirvShaderTranslator::SystemConstants`
// (the UBO the translated SPIR-V binds at DescriptorSet 1, Binding 0) out of
// `VulkanCommandProcessor::UpdateSystemConstantValues`
// (extern/xenia/src/xenia/gpu/vulkan/vulkan_command_processor.cc), reading from
// a register-file snapshot our command processor writes at a real draw
// (runtime/vd_null_gpu.cpp, GEARS_CONST_DUMP -> scratch/bin/regfile_hotpair.bin).
//
// The struct layout and the field-by-field logic are Xenia's (BSD-3-Clause; the
// fork is attributed in README.md / docs/xenia-reuse.md). We only reproduce the
// derivation reading from OUR tracked register file, and reuse Xenia's own
// `draw_util::GetHostViewportInfo` for the NDC scale/offset so that part is not
// an approximation but literally Xenia's code.
//
// It targets the non-FSI, host-render-targets path -- the same configuration
// the SPIR-V modules in this project were translated for
// (tools/xenos_translate: edram_fragment_shader_interlock=false). In that path
// every edram_* / FSI field of the struct stays zero, exactly as Xenia leaves
// them, so they are not filled here.
//
// Inputs the register file does NOT expose are stubbed VISIBLY and reported,
// never invented: the per-texture signedness / swizzle / integer-scale fields
// (texture-cache state) and the manual-index-load fields (draw-parameter state
// owned by the primitive processor). None of those affect geometry placement.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/xenos.h"

namespace {

using namespace xe::gpu;  // XE_GPU_REG_* register-index enumerators.
using xe::gpu::RegisterFile;
using xe::gpu::SpirvShaderTranslator;
namespace draw_util = xe::gpu::draw_util;
namespace reg = xe::gpu::reg;
namespace xenos = xe::gpu::xenos;

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
}

// What the register file cannot tell us, tracked so the report is honest.
struct Stubs {
  // Draw-parameter state (primitive processor) -- owned by draw-params.
  bool index_load_flags = false;      // kSysFlag_(ComputeOrPrimitive)VertexIndexLoad
  uint32_t vertex_index_load_address = 0;
  xenos::Endian vertex_index_endian = xenos::Endian::kNone;
  // Texture-cache state.
  bool texture_signs = false;
  bool texture_swizzles = false;
  bool textures_resolved = false;
  bool texture_integer_scale = false;
  // VGT_DRAW_INITIATOR (register 0x21FC) is written by the DRAW_INDX packet,
  // which our command processor does not yet mirror into the register file
  // (draw-params territory). When it reads as zero the prim-type-derived flag
  // bits (PrimitivePolygonal / PrimitiveLine) and index_size are underdetermined.
  bool draw_initiator_stale = false;
};

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path in;
  std::filesystem::path out;
  // Optional: VGT_DRAW_INITIATOR (register 0x21FC) is set by the DRAW_INDX
  // packet, which our command processor does not mirror into the register file
  // (draw-params owns it), so it reads stale-zero in the snapshot. Pass the
  // value the draw-params capture measured for this same draw
  // (scratch/draw-params/hot_draw.txt) to complete the prim-type-derived fields.
  // This injects a MEASURED value, never an invented one.
  bool have_initiator_override = false;
  uint32_t initiator_override = 0;
  std::vector<std::filesystem::path> pos;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    const std::string kFlag = "--draw-initiator=";
    if (a.rfind(kFlag, 0) == 0) {
      initiator_override = uint32_t(std::stoul(a.substr(kFlag.size()), nullptr, 0));
      have_initiator_override = true;
    } else {
      pos.push_back(a);
    }
  }
  in = pos.size() >= 1 ? pos[0] : std::filesystem::path("scratch/bin/regfile_hotpair.bin");
  out = pos.size() >= 2 ? pos[1] : std::filesystem::path("scratch/bin/system_constants.bin");

  std::vector<uint8_t> raw = ReadFile(in);
  if (raw.size() < RegisterFile::kRegisterCount * sizeof(uint32_t)) {
    std::fprintf(stderr,
                 "%s: %zu bytes, need at least %zu (0x%zx dwords)\n",
                 in.string().c_str(), raw.size(),
                 RegisterFile::kRegisterCount * sizeof(uint32_t),
                 size_t(RegisterFile::kRegisterCount));
    return 2;
  }

  // Load our tracked register dwords into a Xenia RegisterFile. Our command
  // processor stores the same little-endian dword layout at the same indices
  // (ALU 0x4000, fetch 0x4800, context regs 0x2000+), and Xenia's RegisterFile
  // spans a prefix (0x5003) of our 0x8000 space, so a straight prefix copy is
  // exactly right.
  RegisterFile regs;
  std::memcpy(regs.values, raw.data(),
              RegisterFile::kRegisterCount * sizeof(uint32_t));

  if (have_initiator_override) {
    regs.values[XE_GPU_REG_VGT_DRAW_INITIATOR] = initiator_override;
    std::printf("[note] VGT_DRAW_INITIATOR overridden to %#010x "
                "(draw-params-measured)\n", initiator_override);
  }

  Stubs stubs;
  SpirvShaderTranslator::SystemConstants sc;
  std::memset(&sc, 0, sizeof(sc));

  // --- Derivation configuration (non-FSI host render targets) ---
  const bool edram_fragment_shader_interlock = false;
  // No resolution scaling on the host-render-targets path by default.
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

  // --- Viewport (NDC scale/offset): Xenia's own draw_util, not a re-derivation.
  draw_util::ViewportInfo viewport_info;
  draw_util::GetViewportInfoArgs gviargs{};
  // x_max/y_max are the HOST device maxViewportDimensions in Xenia. Our
  // viewport bounds (offset +/- |scale|) sit far inside any real value, so the
  // clamp never bites and the choice does not affect the result; use a typical
  // desktop maximum.
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

  // ================= UpdateSystemConstantValues (non-FSI) =================
  // Faithful reproduction of vulkan_command_processor.cc:UpdateSystemConstantValues.

  // Flags.
  uint32_t flags = 0;
  // Vertex index shader loading -- draw-parameter state (shader_32bit_index_dma
  // and primitive_processing_result.index_buffer_type). Not available offline.
  // For this title's hot kVertex shader (reads gl_VertexIndex) these are not
  // set; the fields they gate are unused by the shader. Left clear, flagged.
  stubs.index_load_flags = true;
  // W0 division control.
  if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (pa_cl_vte_cntl.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  // primitive_polygonal / IsPrimitiveLine read VGT_DRAW_INITIATOR.prim_type,
  // which is draw-parameter state (set by DRAW_INDX). If the register is stale
  // (zero) these bits are underdetermined -- a full-screen pass is typically a
  // kRectangleList, which IS polygonal, so the real draw likely sets the
  // PrimitivePolygonal bit. Computed from the register here, flagged if stale.
  if (vgt_draw_initiator.value == 0) stubs.draw_initiator_stale = true;
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
  // Gamma writing (non-FSI branch).
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
  }
  sc.flags = flags;

  // Index buffer address for loading in the shaders (only if index-load flags).
  // Gated off here (flags don't carry them); value is draw-parameter state.
  sc.vertex_index_load_address = stubs.vertex_index_load_address;

  // Index endianness -- primitive_processing_result.host_shader_index_endian,
  // draw-parameter state. Unused by the hot kVertex shader.
  sc.vertex_index_endian = stubs.vertex_index_endian;

  // Vertex index offset (VGT_INDX_OFFSET).
  sc.vertex_base_index = vgt_indx_offset;

  // Host normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    sc.ndc_scale[i] = viewport_info.ndc_scale[i];
    sc.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size (only for point-list primitives).
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

  // Texture signedness / swizzle / integer scale -- texture-cache state, not in
  // the register file. Left zero, flagged. Do not affect geometry placement.
  stubs.texture_signs = true;
  stubs.texture_swizzles = true;
  stubs.texture_integer_scale = true;
  stubs.textures_resolved = true;

  // Alpha test reference (RB_ALPHA_REF).
  sc.alpha_test_reference = rb_alpha_ref;

  // Alpha to coverage (RB_COLORCONTROL).
  sc.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable
                         ? (rb_colorcontrol.value >> 24) | (1 << 8)
                         : 0;

  // FSI ZPD counter -- non-FSI: sentinel.
  sc.zpd_fsi_counter_index = UINT32_MAX;

  // Color exponent bias. On the host-render-targets path the k_16_16 /
  // k_16_16_16_16 branch also consults render_target_cache truncation state; it
  // is only reached for those two formats. For any other format (this draw is
  // k_2_10_10_10_FLOAT) the value is exactly the register's color_exp_bias.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info =
        regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      // Would depend on render_target_cache IsFixed*Truncated state; not
      // reachable for this draw's format. Note if ever hit.
      std::fprintf(stderr,
                   "WARNING: RT%u is a 16_16 format; color_exp_bias may need "
                   "render_target_cache truncation state (not modeled)\n", i);
    }
    float color_exp_bias_scale;
    int32_t bits = int32_t(0x3F800000) + (color_exp_bias << 23);
    std::memcpy(&color_exp_bias_scale, &bits, sizeof(bits));
    sc.color_exp_bias[i] = color_exp_bias_scale;
  }

  // All edram_* / stencil / poly-offset / blend-constant fields belong to the
  // FSI path only and stay zero here (as Xenia leaves them when !FSI).
  (void)edram_fragment_shader_interlock;

  // ---- Dump the bytes ----
  std::filesystem::create_directories(out.parent_path());
  std::ofstream of(out, std::ios::binary);
  of.write(reinterpret_cast<const char*>(&sc), sizeof(sc));
  of.close();

  // ---- Report ----
  auto pcl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  std::printf("system-constants UBO for %s\n", in.string().c_str());
  std::printf("  sizeof(SystemConstants) = %zu bytes  -> %s\n", sizeof(sc),
              out.string().c_str());
  std::printf("\n--- register inputs ---\n");
  std::printf("  PA_CL_VTE_CNTL   = %#010x  (xy_fmt=%u z_fmt=%u w0_fmt=%u, "
              "vport ena x[s%u o%u] y[s%u o%u] z[s%u o%u])\n",
              pa_cl_vte_cntl.value, pa_cl_vte_cntl.vtx_xy_fmt,
              pa_cl_vte_cntl.vtx_z_fmt, pa_cl_vte_cntl.vtx_w0_fmt,
              pa_cl_vte_cntl.vport_x_scale_ena, pa_cl_vte_cntl.vport_x_offset_ena,
              pa_cl_vte_cntl.vport_y_scale_ena, pa_cl_vte_cntl.vport_y_offset_ena,
              pa_cl_vte_cntl.vport_z_scale_ena, pa_cl_vte_cntl.vport_z_offset_ena);
  std::printf("  PA_CL_CLIP_CNTL  = %#010x  (clip_disable=%u dx_clip_space_def=%u)\n",
              pcl.value, pcl.clip_disable, pcl.dx_clip_space_def);
  std::printf("  VPORT scale/off  = X(%.1f,%.1f) Y(%.1f,%.1f) Z(%.1f,%.1f)\n",
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE),
              regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET));
  std::printf("  RB_SURFACE_INFO  = %#010x  (surface_pitch=%u msaa=%u)\n",
              rb_surface_info.value, rb_surface_info.surface_pitch,
              uint32_t(rb_surface_info.msaa_samples));
  std::printf("  VGT_DRAW_INITIATOR=%#010x (prim_type=%u)\n",
              vgt_draw_initiator.value, uint32_t(vgt_draw_initiator.prim_type));
  std::printf("  RB_COLORCONTROL  = %#010x  (alpha_test_en=%u func=%u a2m_en=%u)\n",
              rb_colorcontrol.value, rb_colorcontrol.alpha_test_enable,
              uint32_t(rb_colorcontrol.alpha_func),
              rb_colorcontrol.alpha_to_mask_enable);
  std::printf("  RB_ALPHA_REF     = %g\n", rb_alpha_ref);
  std::printf("  VGT_INDX_OFFSET  = %d\n", vgt_indx_offset);

  std::printf("\n--- viewport (Xenia draw_util::GetHostViewportInfo) ---\n");
  std::printf("  xy_offset=(%u,%u) xy_extent=(%u,%u) z=[%.4f,%.4f]\n",
              viewport_info.xy_offset[0], viewport_info.xy_offset[1],
              viewport_info.xy_extent[0], viewport_info.xy_extent[1],
              viewport_info.z_min, viewport_info.z_max);
  std::printf("  ndc_scale =(%g, %g, %g)\n", viewport_info.ndc_scale[0],
              viewport_info.ndc_scale[1], viewport_info.ndc_scale[2]);
  std::printf("  ndc_offset=(%g, %g, %g)\n", viewport_info.ndc_offset[0],
              viewport_info.ndc_offset[1], viewport_info.ndc_offset[2]);

  auto off = [&](const void* p) {
    return size_t(reinterpret_cast<const uint8_t*>(p) -
                  reinterpret_cast<const uint8_t*>(&sc));
  };
  std::printf("\n--- SystemConstants fields (offset : value) ---\n");
  std::printf("  [+%3zu] flags                    = %#010x\n", off(&sc.flags), sc.flags);
  std::printf("  [+%3zu] vertex_index_load_address= %#010x   (STUB: draw-params)\n",
              off(&sc.vertex_index_load_address), sc.vertex_index_load_address);
  std::printf("  [+%3zu] vertex_index_endian      = %u          (STUB: draw-params)\n",
              off(&sc.vertex_index_endian), uint32_t(sc.vertex_index_endian));
  std::printf("  [+%3zu] vertex_base_index        = %d\n",
              off(&sc.vertex_base_index), sc.vertex_base_index);
  std::printf("  [+%3zu] ndc_scale                = (%g, %g, %g)\n",
              off(sc.ndc_scale), sc.ndc_scale[0], sc.ndc_scale[1], sc.ndc_scale[2]);
  std::printf("  [+%3zu] ndc_offset               = (%g, %g, %g)\n",
              off(sc.ndc_offset), sc.ndc_offset[0], sc.ndc_offset[1], sc.ndc_offset[2]);
  std::printf("  [+%3zu] alpha_test_reference     = %g\n",
              off(&sc.alpha_test_reference), sc.alpha_test_reference);
  std::printf("  [+%3zu] alpha_to_mask            = %#010x\n",
              off(&sc.alpha_to_mask), sc.alpha_to_mask);
  std::printf("  [+%3zu] zpd_fsi_counter_index    = %#010x\n",
              off(&sc.zpd_fsi_counter_index), sc.zpd_fsi_counter_index);
  std::printf("  [+%3zu] color_exp_bias           = (%g, %g, %g, %g)\n",
              off(sc.color_exp_bias), sc.color_exp_bias[0], sc.color_exp_bias[1],
              sc.color_exp_bias[2], sc.color_exp_bias[3]);
  std::printf("  [+%3zu] texture_swizzled_signs   = (STUB: texture-cache, zero)\n",
              off(sc.texture_swizzled_signs));
  std::printf("  [+%3zu] texture_swizzles         = (STUB: texture-cache/host, zero)\n",
              off(sc.texture_swizzles));
  std::printf("  [+%3zu] textures_resolved        = %#010x   (STUB: texture-cache)\n",
              off(&sc.textures_resolved), sc.textures_resolved);
  std::printf("  [+%3zu] texture_integer_scale_bits (STUB: texture-cache, zero)\n",
              off(sc.texture_integer_scale_bits));
  std::printf("  [+%3zu] edram_* / stencil / blend  (non-FSI path: all zero)\n",
              off(&sc.edram_32bpp_tile_pitch_dwords_scaled));

  std::printf("\n--- stubbed / underdetermined ---\n");
  if (stubs.index_load_flags)
    std::printf("  * index-load flags + vertex_index_load_address + vertex_index_endian:\n"
                "    draw-parameter state (primitive processor / VGT_DRAW_INITIATOR index_size).\n"
                "    Unused by this title's hot kVertex shader (reads gl_VertexIndex).\n");
  if (stubs.texture_signs || stubs.texture_swizzles || stubs.texture_integer_scale ||
      stubs.textures_resolved)
    std::printf("  * texture_swizzled_signs / texture_swizzles / textures_resolved /\n"
                "    texture_integer_scale_bits: texture-cache state (per-texture, derived\n"
                "    from fetch constants + host format), not geometry. Left zero.\n");
  if (stubs.draw_initiator_stale)
    std::printf("  * VGT_DRAW_INITIATOR reads 0 (not yet mirrored from DRAW_INDX by our\n"
                "    command processor): flags bits PrimitivePolygonal / PrimitiveLine and\n"
                "    the index_size path are UNDETERMINED. This full-screen pass is very\n"
                "    likely a kRectangleList (polygonal) -> real flags probably +0x40.\n");

  return 0;
}
