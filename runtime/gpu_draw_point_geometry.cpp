// Xenos point-list expansion. Kept separate from shader translation because a
// point is a primitive operation: it consumes translated VS outputs and emits
// the triangle strip consumed by the translated PS.
#include "gpu_draw_xlate.h"

#ifdef GEARS_HAVE_GUEST_DRAW

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>

#include "xenia/base/math.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"

namespace gears::draw
{
namespace
{

using xe::gpu::SpirvShaderTranslator;

enum PointConstant : uint8_t
{
    kPointConstantDiameter,
    kPointScreenDiameterToNdcRadius,
    kPointConstantCount,
};

} // namespace

bool DerivePointGeometryShaderKey(uint64_t vsModification, uint64_t psModification,
                                  GeometryShaderKey &out)
{
    const SpirvShaderTranslator::Modification vs(vsModification);
    const SpirvShaderTranslator::Modification ps(psModification);
    if (vs.vertex.host_vertex_shader_type != xe::gpu::Shader::HostVertexShaderType::kVertex)
        return false;

    out = {};
    out.type = GeometryShaderType::PointList;
    out.interpolatorCount = xe::bit_count(vs.vertex.interpolator_mask);
    out.hasPointSize = vs.vertex.output_point_parameters;
    out.hasPointCoordinates = ps.pixel.param_gen_point;
    if (vs.vertex.user_clip_plane_cull)
        out.cullDistanceCount = vs.vertex.user_clip_plane_count;
    else
        out.clipDistanceCount = vs.vertex.user_clip_plane_count;
    return true;
}

bool BuildPointGeometryShader(const GeometryShaderKey &key, std::vector<uint32_t> &spirv)
{
    constexpr uint32_t kInputVertexCount = 1;
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
    const spv::Id typeFloat = builder.makeFloatType(32);
    const spv::Id typeFloat2 = builder.makeVectorType(typeFloat, 2);
    const spv::Id typeFloat4 = builder.makeVectorType(typeFloat, 4);
    const spv::Id typeClipDistances =
        clipDistanceCount
            ? builder.makeArrayType(typeFloat, builder.makeUintConstant(clipDistanceCount), 0)
            : spv::NoType;
    const spv::Id typeCullDistances =
        cullDistanceCount
            ? builder.makeArrayType(typeFloat, builder.makeUintConstant(cullDistanceCount), 0)
            : spv::NoType;
    const spv::Id constInputVertexCount = builder.makeUintConstant(kInputVertexCount);
    const spv::Id constInt0 = builder.makeIntConstant(0);
    const spv::Id constInt1 = builder.makeIntConstant(1);
    const spv::Id constFloat0 = builder.makeFloatConstant(0.0f);

    // The geometry stage reads the same system UBO as the translated shaders.
    ids.assign(kPointConstantCount, spv::NoType);
    ids[kPointConstantDiameter] = typeFloat2;
    ids[kPointScreenDiameterToNdcRadius] = typeFloat2;
    const spv::Id typeSystemConstants = builder.makeStructType(ids, "XeSystemConstants");
    builder.addMemberName(typeSystemConstants, kPointConstantDiameter, "point_constant_diameter");
    builder.addMemberDecoration(
        typeSystemConstants, kPointConstantDiameter, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, point_constant_diameter)));
    builder.addMemberName(typeSystemConstants, kPointScreenDiameterToNdcRadius,
                          "point_screen_diameter_to_ndc_radius");
    builder.addMemberDecoration(
        typeSystemConstants, kPointScreenDiameterToNdcRadius, spv::DecorationOffset,
        int(offsetof(SpirvShaderTranslator::SystemConstants, point_screen_diameter_to_ndc_radius)));
    builder.addDecoration(typeSystemConstants, spv::DecorationBlock);
    const spv::Id systemConstants =
        builder.createVariable(spv::NoPrecision, spv::StorageClassUniform, typeSystemConstants,
                               "xe_uniform_system_constants");
    builder.addDecoration(systemConstants, spv::DecorationDescriptorSet,
                          int(SpirvShaderTranslator::kDescriptorSetConstants));
    builder.addDecoration(systemConstants, spv::DecorationBinding,
                          int(SpirvShaderTranslator::kConstantBufferSystem));

    std::vector<spv::Id> mainInterface;

    // in gl_PerVertex gl_in[1].
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
    const spv::Id typeInPerVertex = builder.makeStructType(ids, "gl_PerVertex");
    builder.addMemberName(typeInPerVertex, memberInPosition, "gl_Position");
    builder.addMemberDecoration(typeInPerVertex, memberInPosition, spv::DecorationBuiltIn,
                                int(spv::BuiltIn::Position));
    if (clipDistanceCount)
    {
        builder.addMemberName(typeInPerVertex, memberInClipDistance, "gl_ClipDistance");
        builder.addMemberDecoration(typeInPerVertex, memberInClipDistance, spv::DecorationBuiltIn,
                                    int(spv::BuiltIn::ClipDistance));
    }
    if (cullDistanceCount)
    {
        builder.addMemberName(typeInPerVertex, memberInCullDistance, "gl_CullDistance");
        builder.addMemberDecoration(typeInPerVertex, memberInCullDistance, spv::DecorationBuiltIn,
                                    int(spv::BuiltIn::CullDistance));
    }
    builder.addDecoration(typeInPerVertex, spv::DecorationBlock);
    const spv::Id inPerVertex = builder.createVariable(
        spv::NoPrecision, spv::StorageClassInput,
        builder.makeArrayType(typeInPerVertex, constInputVertexCount, 0), "gl_in");
    mainInterface.push_back(inPerVertex);

    // User locations match the translated VS and PS: interpolators first,
    // followed by point size on input and point coordinates on output.
    std::vector<spv::Id> outInterpolators(key.interpolatorCount);
    std::vector<spv::Id> inInterpolators(key.interpolatorCount);
    for (uint32_t i = 0; i < key.interpolatorCount; ++i)
    {
        outInterpolators[i] =
            builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, typeFloat4,
                                   ("xe_out_interpolator_" + std::to_string(i)).c_str());
        builder.addDecoration(outInterpolators[i], spv::DecorationLocation, int(i));
        builder.addDecoration(outInterpolators[i], spv::DecorationInvariant);
        mainInterface.push_back(outInterpolators[i]);

        inInterpolators[i] =
            builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                                   builder.makeArrayType(typeFloat4, constInputVertexCount, 0),
                                   ("xe_in_interpolator_" + std::to_string(i)).c_str());
        builder.addDecoration(inInterpolators[i], spv::DecorationLocation, int(i));
        mainInterface.push_back(inInterpolators[i]);
    }
    spv::Id outPointCoordinates = spv::NoResult;
    if (key.hasPointCoordinates)
    {
        outPointCoordinates = builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                                     typeFloat2, "xe_out_point_coordinates");
        builder.addDecoration(outPointCoordinates, spv::DecorationLocation,
                              int(key.interpolatorCount));
        builder.addDecoration(outPointCoordinates, spv::DecorationInvariant);
        mainInterface.push_back(outPointCoordinates);
    }
    spv::Id inPointSize = spv::NoResult;
    if (key.hasPointSize)
    {
        inPointSize = builder.createVariable(
            spv::NoPrecision, spv::StorageClassInput,
            builder.makeArrayType(typeFloat, constInputVertexCount, 0), "xe_in_point_size");
        builder.addDecoration(inPointSize, spv::DecorationLocation, int(key.interpolatorCount));
        mainInterface.push_back(inPointSize);
    }

    // out gl_PerVertex. Cull distances are consumed, while clip distances are
    // forwarded unchanged for the expanded sprite.
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
    const spv::Id typeOutPerVertex = builder.makeStructType(ids, "gl_PerVertex");
    builder.addMemberName(typeOutPerVertex, memberOutPosition, "gl_Position");
    builder.addMemberDecoration(typeOutPerVertex, memberOutPosition, spv::DecorationBuiltIn,
                                int(spv::BuiltIn::Position));
    if (clipDistanceCount)
    {
        builder.addMemberName(typeOutPerVertex, memberOutClipDistance, "gl_ClipDistance");
        builder.addMemberDecoration(typeOutPerVertex, memberOutClipDistance, spv::DecorationBuiltIn,
                                    int(spv::BuiltIn::ClipDistance));
    }
    builder.addDecoration(typeOutPerVertex, spv::DecorationBlock);
    const spv::Id outPerVertex =
        builder.createVariable(spv::NoPrecision, spv::StorageClassOutput, typeOutPerVertex, "");
    builder.addDecoration(outPerVertex, spv::DecorationInvariant);
    mainInterface.push_back(outPerVertex);

    std::vector<spv::Id> mainParamTypes;
    std::vector<std::vector<spv::Decoration>> mainPrecisions;
    spv::Block *mainEntry = nullptr;
    spv::Function *mainFunction = builder.makeFunctionEntry(
        spv::NoPrecision, typeVoid, "main", mainParamTypes, mainPrecisions, &mainEntry);
    spv::Instruction *entryPoint =
        builder.addEntryPoint(spv::ExecutionModelGeometry, mainFunction, "main");
    for (spv::Id id : mainInterface)
        entryPoint->addIdOperand(id);
    builder.addExecutionMode(mainFunction, spv::ExecutionModeInputPoints);
    builder.addExecutionMode(mainFunction, spv::ExecutionModeInvocations, 1);
    builder.addExecutionMode(mainFunction, spv::ExecutionModeOutputTriangleStrip);
    builder.addExecutionMode(mainFunction, spv::ExecutionModeOutputVertices,
                             int(kOutputMaxVertices));

    auto discardIf = [&](spv::Id condition)
    {
        spv::Block &predecessor = *builder.getBuildPoint();
        spv::Block &thenBlock = builder.makeNewBlock();
        spv::Block &mergeBlock = builder.makeNewBlock();
        builder.createSelectionMerge(&mergeBlock, spv::SelectionControlDontFlattenMask);
        auto branch = std::make_unique<spv::Instruction>(spv::OpBranchConditional);
        branch->addIdOperand(condition);
        branch->addIdOperand(thenBlock.getId());
        branch->addIdOperand(mergeBlock.getId());
        branch->addImmediateOperand(1);
        branch->addImmediateOperand(2);
        predecessor.addInstruction(std::move(branch));
        thenBlock.addPredecessor(&predecessor);
        mergeBlock.addPredecessor(&predecessor);
        builder.setBuildPoint(&thenBlock);
        builder.createNoResultOp(spv::OpReturn);
        builder.setBuildPoint(&mergeBlock);
    };

    ids = {constInt0, constMemberInPosition};
    const spv::Id pointPosition = builder.createLoad(
        builder.createAccessChain(spv::StorageClassInput, inPerVertex, ids), spv::NoPrecision);
    discardIf(builder.createUnaryOp(spv::OpAny, typeBool,
                                    builder.createUnaryOp(spv::OpIsNan, typeBool4, pointPosition)));

    if (cullDistanceCount)
    {
        const spv::Id constMemberInCullDistance =
            builder.makeIntConstant(int32_t(memberInCullDistance));
        spv::Id cullCondition = spv::NoResult;
        for (uint32_t i = 0; i < cullDistanceCount; ++i)
        {
            ids = {constInt0, constMemberInCullDistance, builder.makeIntConstant(int32_t(i))};
            const spv::Id negative = builder.createBinOp(
                spv::OpFOrdLessThan, typeBool,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassInput, inPerVertex, ids),
                    spv::NoPrecision),
                constFloat0);
            cullCondition =
                cullCondition == spv::NoResult
                    ? negative
                    : builder.createBinOp(spv::OpLogicalAnd, typeBool, cullCondition, negative);
        }
        discardIf(cullCondition);
    }

    auto loadSystemComponent = [&](PointConstant member, spv::Id component)
    {
        ids = {builder.makeIntConstant(int32_t(member)), component};
        return builder.createLoad(
            builder.createAccessChain(spv::StorageClassUniform, systemConstants, ids),
            spv::NoPrecision);
    };
    spv::Id diameterX = loadSystemComponent(kPointConstantDiameter, constInt0);
    spv::Id diameterY = loadSystemComponent(kPointConstantDiameter, constInt1);
    if (key.hasPointSize)
    {
        ids = {constInt0};
        const spv::Id vertexDiameter = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, inPointSize, ids), spv::NoPrecision);
        const spv::Id written =
            builder.createBinOp(spv::OpFOrdGreaterThanEqual, typeBool, vertexDiameter, constFloat0);
        diameterX =
            builder.createTriOp(spv::OpSelect, typeFloat, written, vertexDiameter, diameterX);
        diameterY =
            builder.createTriOp(spv::OpSelect, typeFloat, written, vertexDiameter, diameterY);
    }
    const spv::Id sizeIsZero = builder.createBinOp(
        spv::OpLogicalOr, typeBool,
        builder.createBinOp(spv::OpFOrdLessThanEqual, typeBool, diameterX, constFloat0),
        builder.createBinOp(spv::OpFOrdLessThanEqual, typeBool, diameterY, constFloat0));
    discardIf(sizeIsZero);

    const spv::Id pointW = builder.createCompositeExtract(pointPosition, typeFloat, 3);
    spv::Id radiusX = builder.createNoContractionBinOp(
        spv::OpFMul, typeFloat, diameterX,
        loadSystemComponent(kPointScreenDiameterToNdcRadius, constInt0));
    spv::Id radiusY = builder.createNoContractionBinOp(
        spv::OpFMul, typeFloat, diameterY,
        loadSystemComponent(kPointScreenDiameterToNdcRadius, constInt1));
    radiusX = builder.createNoContractionBinOp(spv::OpFMul, typeFloat, radiusX, pointW);
    radiusY = builder.createNoContractionBinOp(spv::OpFMul, typeFloat, radiusY, pointW);

    const spv::Id pointX = builder.createCompositeExtract(pointPosition, typeFloat, 0);
    const spv::Id pointY = builder.createCompositeExtract(pointPosition, typeFloat, 1);
    const spv::Id pointZ = builder.createCompositeExtract(pointPosition, typeFloat, 2);
    const spv::Id edgeX[2] = {
        builder.createNoContractionBinOp(spv::OpFSub, typeFloat, pointX, radiusX),
        builder.createNoContractionBinOp(spv::OpFAdd, typeFloat, pointX, radiusX)};
    const spv::Id edgeY[2] = {
        builder.createNoContractionBinOp(spv::OpFSub, typeFloat, pointY, radiusY),
        builder.createNoContractionBinOp(spv::OpFAdd, typeFloat, pointY, radiusY)};

    std::vector<spv::Id> pointInterpolators(key.interpolatorCount);
    ids = {constInt0};
    for (uint32_t i = 0; i < key.interpolatorCount; ++i)
        pointInterpolators[i] = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, inInterpolators[i], ids),
            spv::NoPrecision);

    spv::Id pointClipDistances = spv::NoResult;
    if (clipDistanceCount)
    {
        ids = {constInt0, constMemberInClipDistance};
        pointClipDistances = builder.createLoad(
            builder.createAccessChain(spv::StorageClassInput, inPerVertex, ids), spv::NoPrecision);
    }

    for (uint32_t i = 0; i < kOutputMaxVertices; ++i)
    {
        for (uint32_t j = 0; j < key.interpolatorCount; ++j)
            builder.createStore(pointInterpolators[j], outInterpolators[j]);
        const uint32_t x = i >> 1;
        const uint32_t y = i & 1;
        if (key.hasPointCoordinates)
        {
            ids = {builder.makeFloatConstant(float(x)), builder.makeFloatConstant(float(y))};
            builder.createStore(builder.makeCompositeConstant(typeFloat2, ids),
                                outPointCoordinates);
        }
        ids = {edgeX[x], edgeY[y], pointZ, pointW};
        const spv::Id position = builder.createCompositeConstruct(typeFloat4, ids);
        ids = {constMemberOutPosition};
        builder.createStore(position,
                            builder.createAccessChain(spv::StorageClassOutput, outPerVertex, ids));
        if (clipDistanceCount)
        {
            ids = {constMemberOutClipDistance};
            builder.createStore(
                pointClipDistances,
                builder.createAccessChain(spv::StorageClassOutput, outPerVertex, ids));
        }
        builder.createNoResultOp(spv::OpEmitVertex);
    }
    builder.createNoResultOp(spv::OpEndPrimitive);
    builder.leaveFunction();

    std::vector<unsigned int> code;
    builder.dump(code);
    spirv.assign(code.begin(), code.end());
    return !spirv.empty();
}

} // namespace gears::draw

#endif // GEARS_HAVE_GUEST_DRAW
