#include "spirv_clamp.h"

#include <cstring>
#include <map>
#include <set>

namespace gears::draw
{

namespace
{

constexpr uint32_t kMagic = 0x07230203u;

// The subset of SPIR-V this needs to recognise.
enum Op : uint16_t
{
    kOpExtInstImport   = 11,
    kOpExtInst         = 12,
    kOpTypeFloat       = 22,
    kOpTypeVector      = 23,
    kOpTypePointer     = 32,
    kOpConstant        = 43,
    kOpConstantComposite = 44,
    kOpFunction        = 54,
    kOpVariable        = 59,
    kOpStore           = 62,
    kOpCompositeExtract = 81,
    kOpCompositeInsert  = 82,
};

constexpr uint32_t kStorageClassOutput = 3;
constexpr uint32_t kGlslStd450NClamp = 81;   // NaN-aware, matching the translator

uint16_t OpcodeOf(uint32_t word) { return uint16_t(word & 0xFFFFu); }
uint16_t WordCountOf(uint32_t word) { return uint16_t(word >> 16); }

// "GLSL.std.450" as SPIR-V literal words, for matching OpExtInstImport's name.
bool NameIsGlslStd450(const uint32_t* words, size_t count)
{
    char text[64] = {};
    const size_t bytes = std::min(count * sizeof(uint32_t), sizeof(text) - 1);
    std::memcpy(text, words, bytes);
    return std::strcmp(text, "GLSL.std.450") == 0;
}

} // namespace

bool ClampFragmentOutputs(std::vector<uint32_t>& spirv, ClampMode mode)
{
    // A header is five words; anything shorter is not a module.
    if (spirv.size() < 5 || spirv[0] != kMagic)
        return false;

    uint32_t bound = spirv[3];
    uint32_t glslExt = 0;
    uint32_t typeFloat32 = 0;
    uint32_t typeVec4 = 0;
    uint32_t constZero = 0, constOne = 0;     // scalar float
    uint32_t constVec4Zero = 0, constVec4One = 0;
    std::set<uint32_t> outputPtrTypes;        // pointer-to-vec4 in Output class
    std::set<uint32_t> outputVars;
    size_t firstFunction = 0;

    // --- pass 1: learn the module's types, constants and outputs -------------
    for (size_t i = 5; i < spirv.size();)
    {
        const uint16_t op = OpcodeOf(spirv[i]);
        const uint16_t len = WordCountOf(spirv[i]);
        if (len == 0 || i + len > spirv.size())
            return false;                     // malformed: refuse, do not guess

        switch (op)
        {
        case kOpExtInstImport:
            if (len >= 3 && NameIsGlslStd450(&spirv[i + 2], size_t(len) - 2))
                glslExt = spirv[i + 1];
            break;
        case kOpTypeFloat:
            if (len == 3 && spirv[i + 2] == 32)
                typeFloat32 = spirv[i + 1];
            break;
        case kOpTypeVector:
            if (len == 4 && typeFloat32 != 0 && spirv[i + 2] == typeFloat32 &&
                spirv[i + 3] == 4)
                typeVec4 = spirv[i + 1];
            break;
        case kOpTypePointer:
            if (len == 4 && spirv[i + 2] == kStorageClassOutput &&
                typeVec4 != 0 && spirv[i + 3] == typeVec4)
                outputPtrTypes.insert(spirv[i + 1]);
            break;
        case kOpConstant:
            if (len == 4 && typeFloat32 != 0 && spirv[i + 1] == typeFloat32)
            {
                if (spirv[i + 3] == 0x00000000u) constZero = spirv[i + 2];
                if (spirv[i + 3] == 0x3F800000u) constOne = spirv[i + 2];
            }
            break;
        case kOpConstantComposite:
            if (len == 7 && typeVec4 != 0 && spirv[i + 1] == typeVec4)
            {
                const uint32_t a = spirv[i + 3], b = spirv[i + 4],
                               c = spirv[i + 5], d = spirv[i + 6];
                if (a == b && b == c && c == d)
                {
                    if (constZero != 0 && a == constZero) constVec4Zero = spirv[i + 2];
                    if (constOne != 0 && a == constOne)   constVec4One = spirv[i + 2];
                }
            }
            break;
        case kOpVariable:
            if (len >= 4 && outputPtrTypes.count(spirv[i + 1]) != 0 &&
                spirv[i + 3] == kStorageClassOutput)
                outputVars.insert(spirv[i + 2]);
            break;
        case kOpFunction:
            if (firstFunction == 0)
                firstFunction = i;
            break;
        default:
            break;
        }
        i += len;
    }

    // Nothing to clamp is not a failure of this function, but it IS a result the
    // caller must be able to distinguish from success -- a pixel shader with no
    // float4 colour output is not something to silently pass through as "clamped".
    if (glslExt == 0 || typeVec4 == 0 || outputVars.empty() || firstFunction == 0)
        return false;

    // --- the constants the clamp needs, minted only if absent ----------------
    std::vector<uint32_t> newConstants;
    auto mintScalar = [&](uint32_t bits, uint32_t& id) {
        if (id != 0)
            return;
        id = bound++;
        newConstants.push_back((4u << 16) | kOpConstant);
        newConstants.push_back(typeFloat32);
        newConstants.push_back(id);
        newConstants.push_back(bits);
    };
    auto mintVec4 = [&](uint32_t scalar, uint32_t& id) {
        if (id != 0)
            return;
        id = bound++;
        newConstants.push_back((7u << 16) | kOpConstantComposite);
        newConstants.push_back(typeVec4);
        newConstants.push_back(id);
        for (int k = 0; k < 4; ++k)
            newConstants.push_back(scalar);
    };
    mintScalar(0x00000000u, constZero);
    mintScalar(0x3F800000u, constOne);
    mintVec4(constZero, constVec4Zero);
    mintVec4(constOne, constVec4One);

    // --- pass 2: rebuild, clamping the value stored to each colour output ----
    std::vector<uint32_t> out;
    out.reserve(spirv.size() + newConstants.size() + outputVars.size() * 8);
    out.insert(out.end(), spirv.begin(), spirv.begin() + 5);

    for (size_t i = 5; i < spirv.size();)
    {
        const uint16_t op = OpcodeOf(spirv[i]);
        const uint16_t len = WordCountOf(spirv[i]);

        // New constants belong with the other type/constant declarations, which
        // means immediately before the first function.
        if (i == firstFunction && !newConstants.empty())
        {
            out.insert(out.end(), newConstants.begin(), newConstants.end());
            newConstants.clear();
        }

        if (op == kOpStore && len >= 3 && outputVars.count(spirv[i + 1]) != 0)
        {
            uint32_t clamped = bound++;
            if (mode == ClampMode::kAlphaOnly)
            {
                // %a  = OpCompositeExtract %float %value 3
                const uint32_t alpha = bound++;
                out.push_back((5u << 16) | kOpCompositeExtract);
                out.push_back(typeFloat32);
                out.push_back(alpha);
                out.push_back(spirv[i + 2]);
                out.push_back(3u);
                // %ca = OpExtInst %float %glsl NClamp %a %zero %one
                const uint32_t clampedAlpha = bound++;
                out.push_back((8u << 16) | kOpExtInst);
                out.push_back(typeFloat32);
                out.push_back(clampedAlpha);
                out.push_back(glslExt);
                out.push_back(kGlslStd450NClamp);
                out.push_back(alpha);
                out.push_back(constZero);
                out.push_back(constOne);
                // %clamped = OpCompositeInsert %v4float %ca %value 3
                out.push_back((6u << 16) | kOpCompositeInsert);
                out.push_back(typeVec4);
                out.push_back(clamped);
                out.push_back(clampedAlpha);
                out.push_back(spirv[i + 2]);
                out.push_back(3u);
            }
            else
            {
                // %clamped = OpExtInst %v4float %glsl NClamp %value %zero %one
                out.push_back((8u << 16) | kOpExtInst);
                out.push_back(typeVec4);
                out.push_back(clamped);
                out.push_back(glslExt);
                out.push_back(kGlslStd450NClamp);
                out.push_back(spirv[i + 2]);
                out.push_back(constVec4Zero);
                out.push_back(constVec4One);
            }
            // OpStore %out %clamped, keeping any memory operands the store had.
            out.push_back(spirv[i]);
            out.push_back(spirv[i + 1]);
            out.push_back(clamped);
            for (uint16_t k = 3; k < len; ++k)
                out.push_back(spirv[i + k]);
        }
        else
        {
            out.insert(out.end(), spirv.begin() + i, spirv.begin() + i + len);
        }
        i += len;
    }

    out[3] = bound;   // the id bound grew
    spirv.swap(out);
    return true;
}

} // namespace gears::draw
