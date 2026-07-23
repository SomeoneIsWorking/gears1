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
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
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
    // The stage's texture descriptor set layout is decided by the shader, not
    // by us: binding i is texture_bindings_[i] (with its own image dimension),
    // and sampler j lands at binding texture_count + j. Carry that out so the
    // host builds a matching VkDescriptorSetLayout per shader.
    out.textures.clear();
    for (const auto& tb : shader.GetTextureBindingsAfterTranslation())
    {
        ShaderTextureBinding b;
        b.fetchConstant = tb.fetch_constant;
        b.dimension = uint32_t(tb.dimension);
        out.textures.push_back(b);
    }
    out.samplers.clear();
    for (const auto& sb : shader.GetSamplerBindingsAfterTranslation())
    {
        ShaderSamplerBinding s;
        s.fetchConstant = sb.fetch_constant;
        s.magFilter = uint32_t(sb.mag_filter);
        s.minFilter = uint32_t(sb.min_filter);
        s.mipFilter = uint32_t(sb.mip_filter);
        s.anisoFilter = uint32_t(sb.aniso_filter);
        out.samplers.push_back(s);
    }
    out.samplerCount = uint32_t(out.samplers.size());
    out.ok = true;
    lucent::info("draw", "translated {} {:#018x}: {} bytes SPIR-V, {} float constants,"
        " {} textures, {} samplers",
        type == xenos::ShaderType::kVertex ? "VS" : "PS", hash, spirv.size(),
        out.floatCount, out.textures.size(), out.samplerCount);
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

// ---------------------------------------------------------------------------
// Guest texture decode. Every piece of hardware knowledge here is Xenia's:
// texture_util::GetSubresourcesFromFetchConstant (extents/mip range from the
// fetch), texture_util::GetGuestTextureLayout (row pitch, slice strides),
// texture_address::Tiled2D/Tiled3D (the tiled address function), FormatInfo
// (block size / bytes per block) and TextureCache::GuestToHostSwizzle
// (component order). What is ours is the loop that walks blocks and the
// guest-format -> host-format table, which mirrors Xenia's Vulkan host format
// table (vulkan_texture_cache.cc) for the formats we support.

namespace
{

// Endianness is a byte permutation within a 2- or 4-byte unit, uniform across
// the whole texture, so it can be applied as an XOR on the source byte offset
// instead of a separate conversion pass. This is exactly what Xenia's load
// shaders do with XeEndianSwap, expressed per byte.
uint32_t EndianOffsetXor(xenos::Endian e)
{
    switch (e)
    {
        case xenos::Endian::k8in16: return 1;
        case xenos::Endian::k8in32: return 3;
        case xenos::Endian::k16in32: return 2;
        default: return 0;
    }
}

struct HostFormat
{
    TexHostFormat fmt = TexHostFormat::kUnsupported;
    // Indexed by GUEST component (0=R 1=G 2=B 3=A), yields the HOST component
    // that carries it -- Xenia's host_format_swizzle.
    uint8_t sw[4] = {0, 1, 2, 3};
    const char* unsupportedWhy = nullptr;
};

#define SW(a, b, c, d) {uint8_t(a), uint8_t(b), uint8_t(c), uint8_t(d)}

HostFormat MapFormat(xenos::TextureFormat f)
{
    using TF = xenos::TextureFormat;
    switch (f)
    {
        // Uncompressed, byte-for-byte after the endian swap.
        case TF::k_8:
        case TF::k_8_A:      return {TexHostFormat::kR8Unorm, SW(0, 0, 0, 0)};
        case TF::k_8_8:      return {TexHostFormat::kR8G8Unorm, SW(0, 1, 1, 1)};
        case TF::k_8_8_8_8:  return {TexHostFormat::kR8G8B8A8Unorm, SW(0, 1, 2, 3)};
        // Guest 5_6_5 packs R in the low bits, which is what Vulkan calls
        // B5G6R5_UNORM_PACK16 -- no component conversion needed. (Xenia maps it
        // to R5G6B5 and swaps R/B inside its load shader instead.)
        case TF::k_5_6_5:    return {TexHostFormat::kR5G6B5Pack16, SW(2, 1, 0, 2)};
        // Guest 1_5_5_5 packs R in the low bits; host A1R5G5B5_UNORM_PACK16 has
        // B there, so guest R lands in host B and vice versa.
        case TF::k_1_5_5_5:  return {TexHostFormat::kA1R5G5B5Pack16, SW(2, 1, 0, 3)};
        case TF::k_2_10_10_10:
            return {TexHostFormat::kA2B10G10R10Pack32, SW(0, 1, 2, 3)};
        case TF::k_16:       return {TexHostFormat::kR16Unorm, SW(0, 0, 0, 0)};
        case TF::k_16_16:    return {TexHostFormat::kR16G16Unorm, SW(0, 1, 1, 1)};
        case TF::k_16_16_16_16:
            return {TexHostFormat::kR16G16B16A16Unorm, SW(0, 1, 2, 3)};
        case TF::k_16_FLOAT: return {TexHostFormat::kR16Sfloat, SW(0, 0, 0, 0)};
        case TF::k_16_16_FLOAT:
            return {TexHostFormat::kR16G16Sfloat, SW(0, 1, 1, 1)};
        case TF::k_16_16_16_16_FLOAT:
            return {TexHostFormat::kR16G16B16A16Sfloat, SW(0, 1, 2, 3)};
        case TF::k_32_FLOAT: return {TexHostFormat::kR32Sfloat, SW(0, 0, 0, 0)};
        case TF::k_32_32_FLOAT:
            return {TexHostFormat::kR32G32Sfloat, SW(0, 1, 1, 1)};
        case TF::k_32_32_32_32_FLOAT:
            return {TexHostFormat::kR32G32B32A32Sfloat, SW(0, 1, 2, 3)};
        // Block-compressed: the host consumes the guest blocks verbatim.
        case TF::k_DXT1:     return {TexHostFormat::kBc1RgbaUnorm, SW(0, 1, 2, 3)};
        case TF::k_DXT2_3:   return {TexHostFormat::kBc2Unorm, SW(0, 1, 2, 3)};
        case TF::k_DXT4_5:   return {TexHostFormat::kBc3Unorm, SW(0, 1, 2, 3)};
        case TF::k_DXT5A:    return {TexHostFormat::kBc4Unorm, SW(0, 0, 0, 0)};
        case TF::k_DXN:      return {TexHostFormat::kBc5Unorm, SW(0, 1, 1, 1)};
        default:
            return {TexHostFormat::kUnsupported, SW(0, 1, 2, 3),
                    "no host format mapping"};
    }
}

#undef SW

} // namespace

bool DeriveViewport(const uint32_t* registerFile, GuestViewport& out)
{
    RegisterFile regs;
    std::memcpy(regs.values, registerFile,
        RegisterFile::kRegisterCount * sizeof(uint32_t));

    reg::RB_DEPTHCONTROL normalized_depth_control =
        draw_util::GetNormalizedDepthControl(regs);
    draw_util::ViewportInfo vi;
    draw_util::GetViewportInfoArgs args{};
    const uint32_t host_max_viewport_dim = 16384;
    args.Setup(1, 1, xe::divisors::MagicDiv(1), xe::divisors::MagicDiv(1),
               /*origin_bottom_left=*/false, host_max_viewport_dim,
               host_max_viewport_dim, /*allow_reverse_z=*/true,
               normalized_depth_control, /*convert_z_to_float24=*/false,
               /*full_float24_in_0_to_1=*/false,
               /*pixel_shader_writes_depth=*/false);
    args.SetupRegisterValues(regs);
    draw_util::GetHostViewportInfo(&args, vi);

    out.x = vi.xy_offset[0];
    out.y = vi.xy_offset[1];
    out.w = vi.xy_extent[0];
    out.h = vi.xy_extent[1];
    out.zMin = vi.z_min;
    out.zMax = vi.z_max;

    draw_util::Scissor sc{};
    draw_util::GetScissor(regs, sc);
    out.scissorX = sc.offset[0];
    out.scissorY = sc.offset[1];
    out.scissorW = sc.extent[0];
    out.scissorH = sc.extent[1];
    return true;
}

bool DeriveSamplerState(const uint32_t* fetch6, const ShaderSamplerBinding& sb,
                        GuestSamplerState& out)
{
    xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, fetch6, sizeof(fetch));
    if (fetch.type != xenos::FetchConstantType::kTexture)
        return false;

    // kUseFetchConst means the shader's fetch instruction deferred the filter
    // to the fetch constant; that is the whole point of the encoding.
    auto pick = [](uint32_t fromShader, xenos::TextureFilter fromFetch) {
        return fromShader == uint32_t(xenos::TextureFilter::kUseFetchConst)
                   ? uint32_t(fromFetch) : fromShader;
    };
    out.magFilter = pick(sb.magFilter, fetch.mag_filter);
    out.minFilter = pick(sb.minFilter, fetch.min_filter);
    out.mipFilter = pick(sb.mipFilter, fetch.mip_filter);

    xenos::ClampMode cx, cy, cz;
    texture_util::GetClampModesForDimension(fetch, cx, cy, cz);
    out.clamp[0] = uint32_t(cx);
    out.clamp[1] = uint32_t(cy);
    out.clamp[2] = uint32_t(cz);

    xenos::AnisoFilter aniso =
        xenos::AnisoFilter(sb.anisoFilter) == xenos::AnisoFilter::kUseFetchConst
            ? fetch.aniso_filter : xenos::AnisoFilter(sb.anisoFilter);
    out.anisoMax = aniso == xenos::AnisoFilter::kDisabled
                       ? 0u : (1u << (uint32_t(aniso) - 1));
    return true;
}

bool DecodeGuestTexture(const uint32_t* fetch6, const uint8_t* guestBase,
                        uint64_t guestSize, bool wantData, GuestTexture& out)
{
    xenos::xe_gpu_texture_fetch_t fetch{};
    std::memcpy(&fetch, fetch6, sizeof(fetch));
    if (fetch.type != xenos::FetchConstantType::kTexture)
        return false;

    uint32_t w1 = 0, h1 = 0, d1 = 0, basePage = 0, mipPage = 0, mipMin = 0, mipMax = 0;
    texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1,
        &basePage, &mipPage, &mipMin, &mipMax);

    out.formatRaw = uint32_t(fetch.format);
    out.baseFormatRaw = uint32_t(xe::gpu::GetBaseFormat(fetch.format));
    out.formatName = xe::gpu::FormatInfo::GetName(uint32_t(fetch.format));
    out.dimension = uint32_t(fetch.dimension);
    out.width = w1 + 1;
    out.height = (fetch.dimension == xenos::DataDimension::k1D) ? 1 : h1 + 1;
    out.depthOrArraySize = d1 + 1;
    out.tiled = fetch.tiled != 0;
    out.packedMips = fetch.packed_mips != 0;
    out.mipMin = mipMin;
    out.mipMax = mipMax;
    out.baseAddress = basePage << 12;
    out.endian = uint32_t(fetch.endianness);
    out.guestSwizzle = fetch.swizzle;

    const xenos::TextureFormat baseFormat = xe::gpu::GetBaseFormat(fetch.format);
    const HostFormat hf = MapFormat(baseFormat);
    out.hostFormat = hf.fmt;
    // Guest swizzle composed with the host format's own component order,
    // exactly as Xenia's TextureCache::GuestToHostSwizzle does it.
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t g = (fetch.swizzle >> (3 * i)) & 0b111;
        out.hostSwizzle[i] = (g >= 4) ? uint8_t(g & 0b101) : hf.sw[g];
    }

    if (basePage == 0)
    {
        out.skipReason = "base level not stored (mip_min_level > 0)";
        return true;
    }
    if (hf.fmt == TexHostFormat::kUnsupported)
    {
        out.skipReason = hf.unsupportedWhy;
        return true;
    }
    if (!wantData)
        return true;

    const xe::gpu::FormatInfo* fi = xe::gpu::FormatInfo::Get(baseFormat);
    if (!fi || !fi->bytes_per_block())
    {
        out.skipReason = "no block size for format";
        return true;
    }
    out.blockWidth = fi->block_width;
    out.blockHeight = fi->block_height;
    out.bytesPerBlock = fi->bytes_per_block();

    const bool is3D = fetch.dimension == xenos::DataDimension::k3D;
    const bool isCube = fetch.dimension == xenos::DataDimension::kCube;
    const uint32_t arraySize = isCube ? 6
                             : (fetch.dimension == xenos::DataDimension::k2DOrStacked
                                    ? out.depthOrArraySize : 1);
    out.layers = is3D ? 1 : arraySize;
    out.depth3D = is3D ? out.depthOrArraySize : 1;

    xe::gpu::texture_util::TextureGuestLayout layout =
        xe::gpu::texture_util::GetGuestTextureLayout(
            fetch.dimension, fetch.pitch, out.width, out.height,
            out.depthOrArraySize, out.tiled, baseFormat,
            /*has_packed_levels=*/out.packedMips, /*has_base=*/true,
            /*max_level=*/0);
    if (!layout.base.row_pitch_bytes)
    {
        out.skipReason = "degenerate guest layout";
        return true;
    }

    out.blocksX = (out.width + out.blockWidth - 1) / out.blockWidth;
    out.blocksY = (out.height + out.blockHeight - 1) / out.blockHeight;
    out.rowPitchBytes = out.blocksX * out.bytesPerBlock;

    const uint32_t pitchBlocks = layout.base.row_pitch_bytes / out.bytesPerBlock;
    const unsigned bpbLog2 = xe::log2_floor(out.bytesPerBlock);
    // For 3D, array_slice_stride_bytes covers the whole volume; the distance
    // between Z slices is the row pitch times the tile-aligned block rows.
    const uint32_t zSliceStrideBytes =
        layout.base.row_pitch_bytes * layout.base.z_slice_stride_block_rows;
    const uint32_t layerStrideBytes =
        is3D ? 0 : layout.base.array_slice_stride_bytes;

    const uint64_t total = uint64_t(out.rowPitchBytes) * out.blocksY *
                           out.depth3D * out.layers;
    if (total == 0 || total > (uint64_t(256) << 20))
    {
        out.skipReason = "implausible decoded size";
        return true;
    }
    // Everything the decode will touch must be inside the guest window; the
    // guest layout's own extent estimate is the upper bound Xenia uses.
    const uint64_t srcSpan = uint64_t(out.baseAddress) +
        uint64_t(layout.base.level_data_extent_bytes);
    if (srcSpan > guestSize)
    {
        out.skipReason = "texture data outside the guest window";
        return true;
    }

    out.data.assign(size_t(total), 0);
    const uint32_t exor = EndianOffsetXor(fetch.endianness);
    const uint8_t* src = guestBase + out.baseAddress;
    uint8_t* dst = out.data.data();
    const uint32_t bpb = out.bytesPerBlock;

    for (uint32_t layer = 0; layer < out.layers; ++layer)
    {
        const uint64_t layerSrc = uint64_t(layer) * layerStrideBytes;
        for (uint32_t z = 0; z < out.depth3D; ++z)
        {
            for (uint32_t by = 0; by < out.blocksY; ++by)
            {
                for (uint32_t bx = 0; bx < out.blocksX; ++bx)
                {
                    uint64_t so;
                    if (out.tiled)
                    {
                        so = is3D
                            ? uint64_t(xe::gpu::texture_address::Tiled3D(
                                  int32_t(bx), int32_t(by), int32_t(z), pitchBlocks,
                                  layout.base.z_slice_stride_block_rows, bpbLog2))
                            : uint64_t(uint32_t(xe::gpu::texture_address::Tiled2D(
                                  int32_t(bx), int32_t(by), pitchBlocks, bpbLog2)));
                    }
                    else
                    {
                        so = uint64_t(z) * zSliceStrideBytes +
                             uint64_t(by) * layout.base.row_pitch_bytes +
                             uint64_t(bx) * bpb;
                    }
                    so += layerSrc;
                    for (uint32_t b = 0; b < bpb; ++b)
                        *dst++ = src[(so + b) ^ exor];
                }
            }
        }
    }
    return true;
}

} // namespace gears::draw

#endif // GEARS_HAVE_GUEST_DRAW
