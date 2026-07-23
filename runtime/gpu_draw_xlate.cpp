// Xenos->SPIR-V translation and system-constants derivation for the guest-draw
// backend. Compiled against Xenia's headers ONLY (no system Vulkan headers --
// they conflict with Xenia's bundled Vulkan-Headers). See gpu_draw_xlate.h.
#include "gpu_draw_xlate.h"

#include <lucent/log.h>

#ifdef GEARS_HAVE_GUEST_DRAW

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>

#include "xenia/base/string_buffer.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/shader.h"
#include "xenia/gpu/spirv_builder.h"
// Maps glslang's scoped SPIR-V enums back onto the flat spv::CapabilityFoo
// names; every one of Xenia's own SPIR-V translation units includes it.
#include "xenia/gpu/spirv_compatibility.h"
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

// Analysed shaders, keyed by microcode hash. One Shader object per microcode is
// how Xenia works too: ucode analysis is what answers "which interpolators does
// this stage write / read", which the modification derivation needs BEFORE any
// translation happens, and the object then holds one translation per
// modification.
xe::gpu::SpirvShader* GetAnalyzedShader(xenos::ShaderType type,
                                        const uint8_t* ucode, size_t size,
                                        uint64_t hash)
{
    static std::unordered_map<uint64_t, std::unique_ptr<xe::gpu::SpirvShader>> cache;
    auto it = cache.find(hash);
    if (it != cache.end())
        return it->second.get();
    if (size == 0 || size % 4 != 0)
    {
        lucent::warn("draw", "microcode size {} not a dword multiple", size);
        return nullptr;
    }
    auto shader = std::make_unique<xe::gpu::SpirvShader>(type, hash,
        reinterpret_cast<const uint32_t*>(ucode), size / 4, std::endian::big);
    xe::StringBuffer disasm;
    shader->AnalyzeUcode(disasm);
    if (!shader->is_ucode_analyzed())
    {
        lucent::warn("draw", "ucode analyze failed for {:#018x}", hash);
        return nullptr;
    }
    return cache.emplace(hash, std::move(shader)).first->second.get();
}

bool TranslateOne(SpirvShaderTranslator& translator, xenos::ShaderType type,
                  const uint8_t* ucode, size_t size, uint64_t hash,
                  uint64_t modification, ShaderXlate& out)
{
    xe::gpu::SpirvShader* shaderPtr = GetAnalyzedShader(type, ucode, size, hash);
    if (!shaderPtr)
        return false;
    xe::gpu::SpirvShader& shader = *shaderPtr;
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
    // Which vertex fetch constants this stage's geometry comes from. The shader
    // decides these, exactly as it decides its texture bindings.
    out.vertexBindings.clear();
    for (const auto& vb : shader.vertex_bindings())
    {
        ShaderVertexBinding v;
        v.fetchConstant = vb.fetch_constant;
        v.strideWords = vb.stride_words;
        out.vertexBindings.push_back(v);
    }
    out.ok = true;
    lucent::info("draw", "translated {} {:#018x}: {} bytes SPIR-V, {} float constants,"
        " {} textures, {} samplers",
        type == xenos::ShaderType::kVertex ? "VS" : "PS", hash, spirv.size(),
        out.floatCount, out.textures.size(), out.samplerCount);
    return true;
}

// The widest translator path (non-FSI host render targets) -- the configuration
// tools/xenos_translate uses and the one the verified .spv were built for.
SpirvShaderTranslator MakeTranslator()
{
    return SpirvShaderTranslator(
        SpirvShaderTranslator::Features(/*all=*/true),
        /*native_2x_msaa_with_attachments=*/true,
        /*native_2x_msaa_no_attachments=*/true,
        /*edram_fragment_shader_interlock=*/false);
}

}  // namespace

bool DeriveShaderModifications(const uint32_t* registerFile,
                               const uint8_t* vsUcode, size_t vsSize, uint64_t vsHash,
                               const uint8_t* psUcode, size_t psSize, uint64_t psHash,
                               uint64_t& vsModification, uint64_t& psModification)
{
    vsModification = 0;
    psModification = 0;
    xe::gpu::SpirvShader* vs =
        GetAnalyzedShader(xenos::ShaderType::kVertex, vsUcode, vsSize, vsHash);
    xe::gpu::SpirvShader* ps =
        GetAnalyzedShader(xenos::ShaderType::kPixel, psUcode, psSize, psHash);
    if (!vs || !ps)
        return false;

    RegisterFile regs;
    std::memcpy(regs.values, registerFile,
        RegisterFile::kRegisterCount * sizeof(uint32_t));
    auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
    auto sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();

    // The set of interpolators the pair actually exchanges: written by the
    // vertex shader AND read by the pixel shader. This is Xenia's
    // VulkanCommandProcessor::IssueDraw computation verbatim.
    uint32_t param_gen_pos = UINT32_MAX;
    const uint32_t interpolator_mask =
        vs->writes_interpolators() &
        ps->GetInterpolatorInputMask(sq_program_cntl, sq_context_misc, param_gen_pos);

    SpirvShaderTranslator translator = MakeTranslator();

    // --- vertex stage (VulkanPipelineCache::GetCurrentVertexShaderModification)
    {
        SpirvShaderTranslator::Modification m(
            translator.GetDefaultVertexShaderModification(
                vs->GetDynamicAddressableRegisterCount(sq_program_cntl.vs_num_reg),
                xe::gpu::Shader::HostVertexShaderType::kVertex));
        m.vertex.interpolator_mask = interpolator_mask;
        auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
        const uint32_t user_clip_planes =
            pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
        m.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
        m.vertex.user_clip_plane_cull =
            uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
        m.vertex.output_point_parameters =
            uint32_t((vs->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                     regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                         xenos::PrimitiveType::kPointList);
        vsModification = m.value;
    }

    // --- pixel stage (VulkanPipelineCache::GetCurrentPixelShaderModification)
    {
        SpirvShaderTranslator::Modification m(
            translator.GetDefaultPixelShaderModification(
                ps->GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg)));
        m.pixel.interpolator_mask = interpolator_mask;
        m.pixel.interpolators_centroid =
            interpolator_mask &
            ~xenos::GetInterpolatorSamplingPattern(
                regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                sq_context_misc.sc_sample_cntl,
                regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
        if (param_gen_pos < xenos::kMaxInterpolators)
        {
            m.pixel.param_gen_enable = 1;
            m.pixel.param_gen_interpolator = param_gen_pos;
            m.pixel.param_gen_point =
                uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                         xenos::PrimitiveType::kPointList);
        }
        // Host render targets (this backend never takes the FSI path).
        using DepthStencilMode = SpirvShaderTranslator::Modification::DepthStencilMode;
        m.pixel.depth_stencil_mode =
            (ps->implicit_early_z_write_allowed() &&
             (!ps->writes_color_target(0) ||
              !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>())))
                ? DepthStencilMode::kEarlyHint
                : DepthStencilMode::kNoModifiers;
        // Vulkan's MIN/MAX blend ops ignore the blend factors; the Xenos applies
        // them. When the destination factor is ONE the source factor can be
        // folded into the shader output instead.
        m.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kOne;
        m.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;
        if (ps->writes_color_target(0))
        {
            auto blend_control =
                regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
            if ((blend_control.color_comb_fcn == xenos::BlendOp::kMin ||
                 blend_control.color_comb_fcn == xenos::BlendOp::kMax) &&
                blend_control.color_srcblend == xenos::BlendFactor::kSrcAlpha &&
                blend_control.color_destblend == xenos::BlendFactor::kOne)
                m.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
            if ((blend_control.alpha_comb_fcn == xenos::BlendOp::kMin ||
                 blend_control.alpha_comb_fcn == xenos::BlendOp::kMax) &&
                blend_control.alpha_srcblend == xenos::BlendFactor::kSrcAlpha &&
                blend_control.alpha_destblend == xenos::BlendFactor::kOne)
                m.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
        }
        psModification = m.value;
    }
    return true;
}

bool TranslateHotPair(const uint32_t* registerFile,
                      const uint8_t* vsUcode, size_t vsSize, uint64_t vsHash,
                      const uint8_t* psUcode, size_t psSize, uint64_t psHash,
                      ShaderXlate& outVs, ShaderXlate& outPs)
{
    uint64_t vsMod = 0, psMod = 0;
    if (!DeriveShaderModifications(registerFile, vsUcode, vsSize, vsHash,
                                   psUcode, psSize, psHash, vsMod, psMod))
        return false;
    SpirvShaderTranslator translator = MakeTranslator();
    bool a = TranslateOne(translator, xenos::ShaderType::kVertex, vsUcode, vsSize,
                          vsHash, vsMod, outVs);
    bool b = TranslateOne(translator, xenos::ShaderType::kPixel, psUcode, psSize,
                          psHash, psMod, outPs);
    return a && b;
}

bool TranslateShader(bool isVertex, const uint8_t* ucode, size_t size,
                     uint64_t hash, uint64_t modification, ShaderXlate& out)
{
    SpirvShaderTranslator translator = MakeTranslator();
    return TranslateOne(translator,
        isVertex ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel,
        ucode, size, hash, modification, out);
}

bool DeriveRectangleGeometryShaderKey(uint64_t vsModification,
                                      RectangleGeometryShaderKey& out)
{
    const SpirvShaderTranslator::Modification m(vsModification);
    // The VS-expansion fallback and the geometry shader are alternatives, never
    // both; if the modification ever asks for the fallback, refusing here is
    // better than silently emitting a shader whose input interface is wrong.
    if (m.vertex.host_vertex_shader_type !=
        xe::gpu::Shader::HostVertexShaderType::kVertex)
    {
        lucent::warn("draw", "rectangle list with host vertex shader type {}",
            uint32_t(m.vertex.host_vertex_shader_type));
        return false;
    }
    out = {};
    out.interpolatorCount = xe::bit_count(m.vertex.interpolator_mask);
    if (m.vertex.user_clip_plane_cull)
        out.cullDistanceCount = m.vertex.user_clip_plane_count;
    else
        out.clipDistanceCount = m.vertex.user_clip_plane_count;
    return true;
}

bool BuildRectangleGeometryShader(const RectangleGeometryShaderKey& key,
                                  std::vector<uint32_t>& spirv)
{
    // A triangle in, a strip of two triangles out -- the guest's three vertices
    // plus the mirrored fourth.
    constexpr uint32_t kInputVertexCount = 3;
    constexpr uint32_t kOutputMaxVertices = 4;

    const uint32_t clipDistanceCount = key.clipDistanceCount;
    const uint32_t cullDistanceCount = key.cullDistanceCount;

    std::vector<spv::Id> ids;

    xe::gpu::SpirvBuilder builder(spv::Spv_1_0,
        (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1, nullptr);
    builder.addCapability(spv::CapabilityGeometry);
    if (clipDistanceCount)
        builder.addCapability(spv::CapabilityClipDistance);
    if (cullDistanceCount)
        builder.addCapability(spv::CapabilityCullDistance);
    builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    builder.setSource(spv::SourceLanguageUnknown, 0);

    const spv::Id typeVoid = builder.makeVoidType();
    const spv::Id typeBool = builder.makeBoolType();
    const spv::Id typeBool4 = builder.makeVectorType(typeBool, 4);
    const spv::Id typeInt = builder.makeIntType(32);
    const spv::Id typeFloat = builder.makeFloatType(32);
    const spv::Id typeFloat4 = builder.makeVectorType(typeFloat, 4);
    const spv::Id typeClipDistances = clipDistanceCount
        ? builder.makeArrayType(typeFloat,
              builder.makeUintConstant(clipDistanceCount), 0)
        : spv::NoType;
    const spv::Id typeCullDistances = cullDistanceCount
        ? builder.makeArrayType(typeFloat,
              builder.makeUintConstant(cullDistanceCount), 0)
        : spv::NoType;

    std::vector<spv::Id> mainInterface;
    const spv::Id constInputVertexCount = builder.makeUintConstant(kInputVertexCount);

    // in gl_PerVertex gl_in[3]. Member order must match the vertex shader's own
    // block, which is position, then clip distances, then cull distances.
    ids.clear();
    const uint32_t memberInPosition = uint32_t(ids.size());
    ids.push_back(typeFloat4);
    const spv::Id constMemberInPosition = builder.makeIntConstant(int32_t(memberInPosition));
    uint32_t memberInClipDistance = UINT32_MAX;
    spv::Id constMemberInClipDistance = spv::NoResult;
    if (clipDistanceCount)
    {
        memberInClipDistance = uint32_t(ids.size());
        ids.push_back(typeClipDistances);
        constMemberInClipDistance = builder.makeIntConstant(int32_t(memberInClipDistance));
    }
    uint32_t memberInCullDistance = UINT32_MAX;
    if (cullDistanceCount)
    {
        memberInCullDistance = uint32_t(ids.size());
        ids.push_back(typeCullDistances);
    }
    const spv::Id typeStructInPerVertex = builder.makeStructType(ids, "gl_PerVertex");
    builder.addMemberName(typeStructInPerVertex, memberInPosition, "gl_Position");
    builder.addMemberDecoration(typeStructInPerVertex, memberInPosition,
        spv::DecorationBuiltIn, int(spv::BuiltIn::Position));
    if (clipDistanceCount)
    {
        builder.addMemberName(typeStructInPerVertex, memberInClipDistance, "gl_ClipDistance");
        builder.addMemberDecoration(typeStructInPerVertex, memberInClipDistance,
            spv::DecorationBuiltIn, int(spv::BuiltIn::ClipDistance));
    }
    if (cullDistanceCount)
    {
        builder.addMemberName(typeStructInPerVertex, memberInCullDistance, "gl_CullDistance");
        builder.addMemberDecoration(typeStructInPerVertex, memberInCullDistance,
            spv::DecorationBuiltIn, int(spv::BuiltIn::CullDistance));
    }
    builder.addDecoration(typeStructInPerVertex, spv::DecorationBlock);
    const spv::Id inPerVertex = builder.createVariable(spv::NoPrecision,
        spv::StorageClassInput,
        builder.makeArrayType(typeStructInPerVertex, constInputVertexCount, 0), "gl_in");
    mainInterface.push_back(inPerVertex);

    // Interpolator outputs, then interpolator inputs -- glslang's declaration
    // order, and the locations the translated vertex and pixel shaders use.
    std::vector<spv::Id> outInterpolators(key.interpolatorCount);
    for (uint32_t i = 0; i < key.interpolatorCount; ++i)
    {
        outInterpolators[i] = builder.createVariable(spv::NoPrecision,
            spv::StorageClassOutput, typeFloat4,
            ("xe_out_interpolator_" + std::to_string(i)).c_str());
        builder.addDecoration(outInterpolators[i], spv::DecorationLocation, int(i));
        builder.addDecoration(outInterpolators[i], spv::DecorationInvariant);
        mainInterface.push_back(outInterpolators[i]);
    }
    std::vector<spv::Id> inInterpolators(key.interpolatorCount);
    for (uint32_t i = 0; i < key.interpolatorCount; ++i)
    {
        inInterpolators[i] = builder.createVariable(spv::NoPrecision,
            spv::StorageClassInput,
            builder.makeArrayType(typeFloat4, constInputVertexCount, 0),
            ("xe_in_interpolator_" + std::to_string(i)).c_str());
        builder.addDecoration(inInterpolators[i], spv::DecorationLocation, int(i));
        mainInterface.push_back(inInterpolators[i]);
    }

    // out gl_PerVertex. Cull distances are consumed here, not forwarded.
    ids.clear();
    const uint32_t memberOutPosition = uint32_t(ids.size());
    ids.push_back(typeFloat4);
    const spv::Id constMemberOutPosition = builder.makeIntConstant(int32_t(memberOutPosition));
    uint32_t memberOutClipDistance = UINT32_MAX;
    spv::Id constMemberOutClipDistance = spv::NoResult;
    if (clipDistanceCount)
    {
        memberOutClipDistance = uint32_t(ids.size());
        ids.push_back(typeClipDistances);
        constMemberOutClipDistance = builder.makeIntConstant(int32_t(memberOutClipDistance));
    }
    const spv::Id typeStructOutPerVertex = builder.makeStructType(ids, "gl_PerVertex");
    builder.addMemberName(typeStructOutPerVertex, memberOutPosition, "gl_Position");
    builder.addMemberDecoration(typeStructOutPerVertex, memberOutPosition,
        spv::DecorationBuiltIn, int(spv::BuiltIn::Position));
    if (clipDistanceCount)
    {
        builder.addMemberName(typeStructOutPerVertex, memberOutClipDistance, "gl_ClipDistance");
        builder.addMemberDecoration(typeStructOutPerVertex, memberOutClipDistance,
            spv::DecorationBuiltIn, int(spv::BuiltIn::ClipDistance));
    }
    builder.addDecoration(typeStructOutPerVertex, spv::DecorationBlock);
    const spv::Id outPerVertex = builder.createVariable(spv::NoPrecision,
        spv::StorageClassOutput, typeStructOutPerVertex, "");
    builder.addDecoration(outPerVertex, spv::DecorationInvariant);
    mainInterface.push_back(outPerVertex);

    std::vector<spv::Id> mainParamTypes;
    std::vector<std::vector<spv::Decoration>> mainPrecisions;
    spv::Block* mainEntry = nullptr;
    spv::Function* mainFunction = builder.makeFunctionEntry(spv::NoPrecision, typeVoid,
        "main", mainParamTypes, mainPrecisions, &mainEntry);
    spv::Instruction* entryPoint =
        builder.addEntryPoint(spv::ExecutionModelGeometry, mainFunction, "main");
    for (spv::Id id : mainInterface)
        entryPoint->addIdOperand(id);
    builder.addExecutionMode(mainFunction, spv::ExecutionModeTriangles);
    builder.addExecutionMode(mainFunction, spv::ExecutionModeInvocations, 1);
    builder.addExecutionMode(mainFunction, spv::ExecutionModeOutputTriangleStrip);
    builder.addExecutionMode(mainFunction, spv::ExecutionModeOutputVertices,
        int(kOutputMaxVertices));

    // Returning early from a geometry shader emits nothing, which is how both
    // the NaN and the cull tests below drop the whole primitive.
    auto discardIf = [&](spv::Id condition) {
        spv::Block& predecessor = *builder.getBuildPoint();
        spv::Block& thenBlock = builder.makeNewBlock();
        spv::Block& mergeBlock = builder.makeNewBlock();
        builder.createSelectionMerge(&mergeBlock, spv::SelectionControlDontFlattenMask);
        {
            auto branch = std::make_unique<spv::Instruction>(spv::OpBranchConditional);
            branch->addIdOperand(condition);
            branch->addIdOperand(thenBlock.getId());
            branch->addIdOperand(mergeBlock.getId());
            branch->addImmediateOperand(1);
            branch->addImmediateOperand(2);
            predecessor.addInstruction(std::move(branch));
        }
        thenBlock.addPredecessor(&predecessor);
        mergeBlock.addPredecessor(&predecessor);
        builder.setBuildPoint(&thenBlock);
        builder.createNoResultOp(spv::OpReturn);
        builder.setBuildPoint(&mergeBlock);
    };

    // A NaN position marks a killed vertex; the whole primitive goes.
    for (uint32_t i = 0; i < kInputVertexCount; ++i)
    {
        ids.clear();
        ids.push_back(builder.makeIntConstant(int32_t(i)));
        ids.push_back(constMemberInPosition);
        discardIf(builder.createUnaryOp(spv::OpAny, typeBool,
            builder.createUnaryOp(spv::OpIsNan, typeBool4,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassInput, inPerVertex, ids),
                    spv::NoPrecision))));
    }

    // Cull the primitive when a cull distance is negative at every vertex.
    if (cullDistanceCount)
    {
        const spv::Id constMemberInCullDistance =
            builder.makeIntConstant(int32_t(memberInCullDistance));
        const spv::Id constFloat0 = builder.makeFloatConstant(0.0f);
        spv::Id cullCondition = spv::NoResult;
        for (uint32_t i = 0; i < cullDistanceCount; ++i)
        {
            for (uint32_t j = 0; j < kInputVertexCount; ++j)
            {
                ids.clear();
                ids.push_back(builder.makeIntConstant(int32_t(j)));
                ids.push_back(constMemberInCullDistance);
                ids.push_back(builder.makeIntConstant(int32_t(i)));
                const spv::Id negative = builder.createBinOp(spv::OpFOrdLessThan, typeBool,
                    builder.createLoad(
                        builder.createAccessChain(spv::StorageClassInput, inPerVertex, ids),
                        spv::NoPrecision),
                    constFloat0);
                cullCondition = cullCondition == spv::NoResult
                    ? negative
                    : builder.createBinOp(spv::OpLogicalAnd, typeBool, cullCondition, negative);
            }
        }
        discardIf(cullCondition);
    }

    // Which of the three edges is the longest decides where the fourth vertex
    // goes -- it is the mirror of the first across the diagonal:
    //
    //   0---1
    //   |  /|   12 longest -> strip 0 1 2 3, v3 = -v0 + v1 + v2
    //   | / |   20 longest -> strip 1 2 0 3
    //   |/  |   01 longest -> strip 2 0 1 3
    //   2--[3]
    //
    // Edge lengths are compared squared and in screen X/Y only, as on Xenia.
    const spv::Id constInt0 = builder.makeIntConstant(0);
    const spv::Id constInt1 = builder.makeIntConstant(1);
    const spv::Id constInt2 = builder.makeIntConstant(2);
    const spv::Id constInt3 = builder.makeIntConstant(3);

    spv::Id edgeLengths[3];
    ids.resize(3);
    ids[1] = constMemberInPosition;
    auto loadPositionComponent = [&](uint32_t vertex, spv::Id component) {
        ids[0] = builder.makeIntConstant(int32_t(vertex));
        ids[2] = component;
        return builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, inPerVertex, ids),
            spv::NoPrecision);
    };
    for (uint32_t i = 0; i < 3; ++i)
    {
        const spv::Id x0 = loadPositionComponent((1 + i) % 3, constInt0);
        const spv::Id y0 = loadPositionComponent((1 + i) % 3, constInt1);
        const spv::Id x1 = loadPositionComponent((2 + i) % 3, constInt0);
        const spv::Id y1 = loadPositionComponent((2 + i) % 3, constInt1);
        const spv::Id ex = builder.createBinOp(spv::OpFSub, typeFloat, x1, x0);
        const spv::Id ey = builder.createBinOp(spv::OpFSub, typeFloat, y1, y0);
        edgeLengths[i] = builder.createBinOp(spv::OpFAdd, typeFloat,
            builder.createBinOp(spv::OpFMul, typeFloat, ex, ex),
            builder.createBinOp(spv::OpFMul, typeFloat, ey, ey));
    }

    spv::Id vertexIndices[3];
    vertexIndices[0] = builder.createTriOp(spv::OpSelect, typeInt,
        builder.createBinOp(spv::OpLogicalAnd, typeBool,
            builder.createBinOp(spv::OpFOrdGreaterThan, typeBool,
                edgeLengths[0], edgeLengths[1]),
            builder.createBinOp(spv::OpFOrdGreaterThan, typeBool,
                edgeLengths[0], edgeLengths[2])),
        constInt0,
        builder.createTriOp(spv::OpSelect, typeInt,
            builder.createBinOp(spv::OpFOrdGreaterThan, typeBool,
                edgeLengths[1], edgeLengths[2]),
            constInt1, constInt2));
    for (uint32_t i = 1; i < 3; ++i)
    {
        const spv::Id unwrapped = builder.createBinOp(spv::OpIAdd, typeInt,
            vertexIndices[0], builder.makeIntConstant(int32_t(i)));
        vertexIndices[i] = builder.createTriOp(spv::OpSelect, typeInt,
            builder.createBinOp(spv::OpSLessThan, typeBool, unwrapped, constInt3),
            unwrapped,
            builder.createBinOp(spv::OpISub, typeInt, unwrapped, constInt3));
    }

    // The three guest vertices, in strip order.
    for (uint32_t i = 0; i < 3; ++i)
    {
        const spv::Id vertexIndex = vertexIndices[i];
        ids.clear();
        ids.push_back(vertexIndex);
        for (uint32_t j = 0; j < key.interpolatorCount; ++j)
        {
            builder.createStore(
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassInput, inInterpolators[j], ids),
                    spv::NoPrecision),
                outInterpolators[j]);
        }
        ids.clear();
        ids.push_back(vertexIndex);
        ids.push_back(constMemberInPosition);
        const spv::Id position = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, inPerVertex, ids),
            spv::NoPrecision);
        ids.clear();
        ids.push_back(constMemberOutPosition);
        builder.createStore(position,
            builder.createAccessChain(spv::StorageClassOutput, outPerVertex, ids));
        if (clipDistanceCount)
        {
            ids.clear();
            ids.push_back(vertexIndex);
            ids.push_back(constMemberInClipDistance);
            const spv::Id clip = builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, inPerVertex, ids),
                spv::NoPrecision);
            ids.clear();
            ids.push_back(constMemberOutClipDistance);
            builder.createStore(clip,
                builder.createAccessChain(spv::StorageClassOutput, outPerVertex, ids));
        }
        builder.createNoResultOp(spv::OpEmitVertex);
    }

    // The fourth: every attribute mirrored the same way the position is,
    // v3 = v2 + (v1 - v0).
    auto mirror = [&](spv::Id type, spv::Id variable, const spv::Id* member,
                      size_t memberCount) {
        auto load = [&](spv::Id vertex) {
            ids.clear();
            ids.push_back(vertex);
            for (size_t i = 0; i < memberCount; ++i)
                ids.push_back(member[i]);
            return builder.createLoad(
                builder.createAccessChain(spv::StorageClassInput, variable, ids),
                spv::NoPrecision);
        };
        const spv::Id v0 = load(vertexIndices[0]);
        const spv::Id v01 = builder.createNoContractionBinOp(spv::OpFSub, type,
            load(vertexIndices[1]), v0);
        return builder.createNoContractionBinOp(spv::OpFAdd, type, v01,
            load(vertexIndices[2]));
    };

    for (uint32_t i = 0; i < key.interpolatorCount; ++i)
    {
        builder.createStore(mirror(typeFloat4, inInterpolators[i], nullptr, 0),
            outInterpolators[i]);
    }
    {
        const spv::Id member[] = {constMemberInPosition};
        const spv::Id position = mirror(typeFloat4, inPerVertex, member, 1);
        ids.clear();
        ids.push_back(constMemberOutPosition);
        builder.createStore(position,
            builder.createAccessChain(spv::StorageClassOutput, outPerVertex, ids));
    }
    for (uint32_t i = 0; i < clipDistanceCount; ++i)
    {
        const spv::Id constI = builder.makeIntConstant(int32_t(i));
        const spv::Id member[] = {constMemberInClipDistance, constI};
        const spv::Id clip = mirror(typeFloat, inPerVertex, member, 2);
        ids.clear();
        ids.push_back(constMemberOutClipDistance);
        ids.push_back(constI);
        builder.createStore(clip,
            builder.createAccessChain(spv::StorageClassOutput, outPerVertex, ids));
    }
    builder.createNoResultOp(spv::OpEmitVertex);
    builder.createNoResultOp(spv::OpEndPrimitive);

    builder.leaveFunction();

    std::vector<unsigned int> code;
    builder.dump(code);
    spirv.assign(code.begin(), code.end());
    return !spirv.empty();
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
