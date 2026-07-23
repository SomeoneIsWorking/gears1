#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Bridge between the Xenos translator (which drags in Xenia's bundled
// Vulkan-Headers) and the guest-draw renderer (which uses the system Vulkan
// headers). The two header sets cannot coexist in one translation unit, so the
// translation and system-constants derivation live in gpu_draw_xlate.cpp behind
// this plain-type interface, and gpu_draw.cpp does the actual Vulkan work.
namespace gears::draw
{

// One texture the translated shader declared, in the SAME order the translator
// assigned binding indices (binding i == index i in this list, within the
// stage's texture descriptor set). Samplers follow the images: sampler j is at
// binding textures.size() + j. This is Xenia's
// SpirvShaderTranslator::FindOrAddTextureBinding/FindOrAddSamplerBinding
// contract, and the host descriptor set layout must match it exactly -- a
// layout with the wrong binding count or image type is undefined behaviour
// (observed: a RADV null-deref inside lower_immediate_samplers).
struct ShaderTextureBinding
{
    uint32_t fetchConstant = 0; // which of the 32 texture fetch constants feeds it
    uint32_t dimension = 1;     // xenos::FetchOpDimension: 0/1 -> 2D array, 2 -> 3D, 3 -> cube
};

// One sampler the translated shader declared, in binding order (sampler j is
// at binding textures.size() + j). Filter fields are xenos::TextureFilter /
// AnisoFilter; the value kUseFetchConst means "take it from the fetch
// constant", which DeriveSamplerState resolves.
struct ShaderSamplerBinding
{
    uint32_t fetchConstant = 0;
    uint32_t magFilter = 3, minFilter = 3, mipFilter = 3, anisoFilter = 0;
};

// One vertex buffer the translated vertex shader fetches from, as the shader
// itself declared it. `fetchConstant` indexes the 96 two-dword VERTEX fetch
// constants that overlay the same 0x4800 file the texture fetches use
// (xe_gpu_vertex_fetch_t: dword0 = type:2 | address:30 in dwords,
// dword1 = endian:2 | size:24 in dwords). The renderer needs this to know which
// guest address range a draw's geometry actually comes from.
struct ShaderVertexBinding
{
    uint32_t fetchConstant = 0; // [0, 96)
    uint32_t strideWords = 0;   // stride of the whole binding, in dwords
};

struct ShaderXlate
{
    bool ok = false;
    std::vector<uint8_t> spirv;      // translated SPIR-V module
    uint64_t floatBitmap[4] = {0, 0, 0, 0}; // ConstantRegisterMap::float_bitmap
    uint32_t floatCount = 0;         // number of float4 constants the UBO holds
    std::vector<ShaderTextureBinding> textures; // binding index == vector index
    std::vector<ShaderSamplerBinding> samplers; // binding textures.size() + index
    std::vector<ShaderVertexBinding> vertexBindings; // vertex stage only
    uint32_t samplerCount = 0;       // == samplers.size(); bindings [textures.size(), +samplerCount)
};

// A shader's translation is NOT a function of its microcode alone. Xenia's
// SpirvShaderTranslator::Modification selects, among other things, WHICH
// INTERPOLATORS the pair passes between the stages -- and that is a property of
// the vertex shader, the pixel shader and the draw's own SQ_PROGRAM_CNTL /
// SQ_CONTEXT_MISC registers together, not of either stage on its own. Translated
// with a zero modification, the vertex shader exports no interpolators at all
// and the pixel shader reads them as zero, so any shader whose colour comes from
// an interpolant (a texture coordinate, a vertex colour) shades pure black while
// position, clipping and rasterisation still look perfectly healthy.
//
// So the modification must be derived per draw from the pair plus the registers,
// exactly as VulkanPipelineCache::GetCurrentVertex/PixelShaderModification does,
// and the caller must cache translations by (hash, modification), not by hash.
bool DeriveShaderModifications(const uint32_t* registerFile,
                               const uint8_t* vsUcode, size_t vsSize, uint64_t vsHash,
                               const uint8_t* psUcode, size_t psSize, uint64_t psHash,
                               uint64_t& vsModification, uint64_t& psModification);

// Translates the bound hot pair's microcode (big-endian bytes) via Xenia's
// front end + SPIR-V back end -- the same path that produced the verified .spv.
// Returns the SPIR-V plus each stage's float-constant map (which real constants
// the packed float UBO holds, and in what order). false if either stage fails.
bool TranslateHotPair(const uint32_t* registerFile,
                      const uint8_t* vsUcode, size_t vsSize, uint64_t vsHash,
                      const uint8_t* psUcode, size_t psSize, uint64_t psHash,
                      ShaderXlate& outVs, ShaderXlate& outPs);

// The rectangle-list geometry shader's shape. Everything in it is derived from
// the draw's own vertex-shader modification, so it is exactly as cacheable as
// the pipeline is.
struct RectangleGeometryShaderKey
{
    uint32_t interpolatorCount = 0;  // how many vec4s the VS/PS pair exchanges
    uint32_t clipDistanceCount = 0;  // gl_ClipDistance array size, 0 if unused
    uint32_t cullDistanceCount = 0;  // gl_CullDistance array size, 0 if unused

    auto operator<=>(const RectangleGeometryShaderKey&) const = default;
};

// A rectangle list gives three vertices per rectangle and the hardware infers
// the fourth by mirroring one across the longest edge. The fourth vertex's
// ATTRIBUTES are derived the same way, so it cannot be synthesized on the CPU
// ahead of the vertex shader -- the expansion has to happen after the vertices
// are shaded. Xenia does it in a geometry shader (its VS-expansion fallback,
// kRectangleListAsTriangleStrip, is an unimplemented TODO in the SPIR-V
// translator), and this is a port of that shader:
// VulkanPipelineCache::GetGeometryShader, kRectangleList branch.
//
// Not ported: the point-list and quad-list branches, the point system-constants
// UBO, gl_PointSize input and the point-coordinates output. All three of the
// point-related key fields are gated in Xenia on the draw's prim_type being
// kPointList, so they are unreachable for a rectangle list rather than being
// left out for convenience.
//
// Deviation from Xenia, deliberate: the clip/cull distance arrays are sized by
// the modification's ACTUAL user_clip_plane_count, not rounded up to 6. Xenia
// rounds up "to reduce variants", but the vertex shader it pairs with declares
// the real count (spirv_shader_translator.cc), so rounding up makes the two
// stages disagree about a built-in array's size.
bool DeriveRectangleGeometryShaderKey(uint64_t vsModification,
                                      RectangleGeometryShaderKey& out);
bool BuildRectangleGeometryShader(const RectangleGeometryShaderKey& key,
                                  std::vector<uint32_t>& spirv);

// Translates a single stage (vertex or pixel) under the given modification --
// the value DeriveShaderModifications produced for the draw's pair. Lets the
// whole-frame backend translate and cache each distinct (shader, modification)
// once.
bool TranslateShader(bool isVertex, const uint8_t* ucode, size_t size,
                     uint64_t hash, uint64_t modification, ShaderXlate& out);

// Derives the system-constants UBO (Xenia's SpirvShaderTranslator::
// SystemConstants) from our tracked register file, returned as raw bytes.
// Ports UpdateSystemConstantValues (non-FSI host-render-targets path).
std::vector<uint8_t> DeriveSystemConstants(const uint32_t* registerFile);

// The draw's own viewport and scissor, in render-target pixels, derived from
// the guest's PA_CL_VPORT_*/PA_SC_* registers by Xenia's draw_util
// (GetHostViewportInfo / GetScissor) -- the same call DeriveSystemConstants
// already makes for the NDC scale/offset, so the two cannot disagree.
struct GuestViewport
{
    uint32_t x = 0, y = 0, w = 0, h = 0;
    float zMin = 0.0f, zMax = 1.0f;
    uint32_t scissorX = 0, scissorY = 0, scissorW = 0, scissorH = 0;
};
bool DeriveViewport(const uint32_t* registerFile, GuestViewport& out);

// --------------------------------------------------------------------------
// Guest texture decode.
//
// A texture fetch constant (6 dwords at 0x4800 + fc*6) fully describes a guest
// texture: base address, dimension, extents, format, tiling, endianness,
// swizzle and mip range. This turns one into a host-uploadable blob using
// Xenia's own texture_info / texture_util / texture_address -- the guest
// layout, the tiled address function and the format table are all Xenia's, not
// a reimplementation.
//
// Only the BASE level is decoded (host mip level 0). Mip tails are not read;
// the caller creates a single-level image and the sampler clamps to it.

// Host format for the decoded blob. Values are ours -- gpu_draw.cpp maps them
// to VkFormat, because the two header sets cannot share a translation unit.
enum class TexHostFormat : uint32_t
{
    kUnsupported = 0,
    kR8Unorm,
    kR8G8Unorm,
    kR8G8B8A8Unorm,
    kR5G6B5Pack16,
    kA1R5G5B5Pack16,
    kB4G4R4A4Pack16,
    kA2B10G10R10Pack32,
    kR16Sfloat,
    kR16G16Sfloat,
    kR16G16B16A16Sfloat,
    kR16Unorm,
    kR16G16Unorm,
    kR16G16B16A16Unorm,
    kR32Sfloat,
    kR32G32Sfloat,
    kR32G32B32A32Sfloat,
    kBc1RgbaUnorm,
    kBc2Unorm,
    kBc3Unorm,
    kBc4Unorm,
    kBc5Unorm,
};

struct GuestTexture
{
    // --- description (always filled when Describe/Decode returns true) ---
    uint32_t formatRaw = 0;        // xenos::TextureFormat as stored in the fetch
    uint32_t baseFormatRaw = 0;    // after texture_info's GetBaseFormat
    const char* formatName = "";   // Xenia's own name for formatRaw
    uint32_t dimension = 0;        // xenos::DataDimension: 0=1D 1=2D/stacked 2=3D 3=cube
    uint32_t width = 0;            // texels
    uint32_t height = 0;
    uint32_t depthOrArraySize = 1; // 3D depth, stacked array size, 6 for cube
    bool tiled = false;
    bool packedMips = false;
    uint32_t mipMin = 0, mipMax = 0;
    uint32_t baseAddress = 0;      // guest physical byte address of the base level
    uint32_t endian = 0;           // xenos::Endian
    uint32_t guestSwizzle = 0;     // raw 12-bit swizzle from the fetch

    TexHostFormat hostFormat = TexHostFormat::kUnsupported;
    // Per-component source, already combining the guest swizzle with the host
    // format's own component order (Xenia TextureCache::GuestToHostSwizzle).
    // Values: 0=R 1=G 2=B 3=A 4=zero 5=one. Index is the destination component.
    uint8_t hostSwizzle[4] = {0, 1, 2, 3};

    // --- decoded payload (only when Decode was asked for data) -----------
    uint32_t blockWidth = 1, blockHeight = 1, bytesPerBlock = 4;
    uint32_t blocksX = 0, blocksY = 0; // base level extent in blocks
    uint32_t layers = 1;               // host array layers (1 for 3D, 6 for cube)
    uint32_t depth3D = 1;              // host image depth (1 unless 3D)
    uint32_t rowPitchBytes = 0;        // tightly packed: blocksX * bytesPerBlock
    std::vector<uint8_t> data;         // layer-major, then z, then rows

    // Set when the fetch describes a texture the decoder deliberately did not
    // upload; the reason is for the census, never for a silent substitution.
    const char* skipReason = nullptr;
};

// Sampler state for one shader sampler binding, resolved against the texture
// fetch constant it names (Xenia texture_util::GetClampModesForDimension plus
// the kUseFetchConst filter fallbacks).
struct GuestSamplerState
{
    uint32_t magFilter = 0;  // xenos::TextureFilter: 0 point, 1 linear
    uint32_t minFilter = 0;
    uint32_t mipFilter = 0;
    uint32_t clamp[3] = {0, 0, 0}; // xenos::ClampMode per axis
    uint32_t anisoMax = 0;         // 0 = anisotropy disabled, else max ratio
};

// Resolves one shader sampler binding against the fetch constant it names.
bool DeriveSamplerState(const uint32_t* fetch6, const ShaderSamplerBinding& sb,
                        GuestSamplerState& out);

// Decodes one texture fetch constant. `fetch6` points at the 6 raw dwords.
// `guestBase`/`guestSize` are the guest physical window (the texture's base
// address is an offset into it). With wantData=false only the description is
// filled (cheap -- used for the frame census). Returns false only when the
// fetch constant is not a texture fetch at all, or names address 0.
bool DecodeGuestTexture(const uint32_t* fetch6, const uint8_t* guestBase,
                        uint64_t guestSize, bool wantData, GuestTexture& out);

} // namespace gears::draw
