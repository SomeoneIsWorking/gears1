#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "gpu_draw_texture_decode.h"

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

// What a VERTEX shader's MICROCODE ALONE says about how it transforms its
// vertices -- no registers, no draw, no translation, so it is a pure function of
// the microcode and is cached by hash exactly as the analysis is.
//
// `floatDynamicAddressing` is the load-bearing field: it is set when the shader
// reads its float constants through the address register (a0) rather than at
// fixed indices, which is what a BONE PALETTE lookup compiles to. A rigid mesh
// has no reason to index its constants dynamically, so this separates a skinned
// character from static world geometry from the microcode itself, rather than
// from a guess at what the constant VALUES look like.
struct VertexShaderShape
{
    bool ok = false;                     // false: the microcode did not analyse
    bool floatDynamicAddressing = false; // indexes float constants through a0
    uint32_t floatCount = 0;             // float4 constants the shader reads
};

VertexShaderShape AnalyzeVertexShaderShape(const uint8_t *ucode, size_t size, uint64_t hash);

struct ShaderXlate
{
    bool ok = false;
    std::vector<uint8_t> spirv;             // translated SPIR-V module
    uint64_t floatBitmap[4] = {0, 0, 0, 0}; // ConstantRegisterMap::float_bitmap
    uint32_t floatCount = 0;                // number of float4 constants the UBO holds
    // Whether the stage reads its float constants through the address register
    // rather than at fixed indices -- a bone-palette lookup, i.e. GPU skinning.
    // Carried here as well as in VertexShaderShape because it decides whether a
    // FIXED CONSTANT LAYOUT can be assumed of a dump: tools/clip_check.py reads
    // c0..c3 as a world matrix and c7..c10 as a view-projection, and on a
    // skinned shader those slots hold bone rows, so the same arithmetic
    // produces confident nonsense. The dump states it so the consumer does not
    // have to guess.
    bool floatDynamicAddressing = false;
    std::vector<ShaderTextureBinding> textures;      // binding index == vector index
    std::vector<ShaderSamplerBinding> samplers;      // binding textures.size() + index
    std::vector<ShaderVertexBinding> vertexBindings; // vertex stage only
    uint32_t samplerCount = 0; // == samplers.size(); bindings [textures.size(), +samplerCount)
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
bool DeriveShaderModifications(const uint32_t *registerFile, const uint8_t *vsUcode, size_t vsSize,
                               uint64_t vsHash, const uint8_t *psUcode, size_t psSize,
                               uint64_t psHash, uint64_t &vsModification, uint64_t &psModification);

// Translates the bound hot pair's microcode (big-endian bytes) via Xenia's
// front end + SPIR-V back end -- the same path that produced the verified .spv.
// Returns the SPIR-V plus each stage's float-constant map (which real constants
// the packed float UBO holds, and in what order). false if either stage fails.
bool TranslateHotPair(const uint32_t *registerFile, const uint8_t *vsUcode, size_t vsSize,
                      uint64_t vsHash, const uint8_t *psUcode, size_t psSize, uint64_t psHash,
                      ShaderXlate &outVs, ShaderXlate &outPs);

// The rectangle-list geometry shader's shape. Everything in it is derived from
// the draw's own vertex-shader modification, so it is exactly as cacheable as
// the pipeline is.
enum class GeometryShaderType : uint32_t
{
    PointList,
    RectangleList,
};

struct GeometryShaderKey
{
    GeometryShaderType type = GeometryShaderType::RectangleList;
    uint32_t interpolatorCount = 0; // how many vec4s the VS/PS pair exchanges
    uint32_t clipDistanceCount = 0; // gl_ClipDistance array size, 0 if unused
    uint32_t cullDistanceCount = 0; // gl_CullDistance array size, 0 if unused
    bool hasPointSize = false;
    bool hasPointCoordinates = false;

    auto operator<=>(const GeometryShaderKey &) const = default;
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
// Deviation from Xenia, deliberate: the clip/cull distance arrays are sized by
// the modification's ACTUAL user_clip_plane_count, not rounded up to 6. Xenia
// rounds up "to reduce variants", but the vertex shader it pairs with declares
// the real count (spirv_shader_translator.cc), so rounding up makes the two
// stages disagree about a built-in array's size.
bool DeriveRectangleGeometryShaderKey(uint64_t vsModification, GeometryShaderKey &out);
bool BuildRectangleGeometryShader(const GeometryShaderKey &key, std::vector<uint32_t> &spirv);

// Xenos points are rectangles whose width and height come from PA_SU_POINT_SIZE
// (or a scalar diameter exported by the vertex shader). Vulkan point primitives
// cannot represent a rectangular size, so Xenia expands each point to a
// four-vertex triangle strip in a geometry shader. Point coordinates, when the
// pixel shader requests PsParamGen, are carried in the location immediately
// following the interpolators.
bool DerivePointGeometryShaderKey(uint64_t vsModification, uint64_t psModification,
                                  GeometryShaderKey &out);
bool BuildPointGeometryShader(const GeometryShaderKey &key, std::vector<uint32_t> &spirv);

// The compute shader that performs a RESOLVE: copies a rectangle from an EDRAM
// surface's host image into a destination texture's host image, applying the
// state the guest attached to the copy.
//
// A resolve cannot be a vkCmdBlitImage, because a blit cannot do the two things
// the guest asks for. RB_COPY_DEST_INFO carries copy_dest_exp_bias, a signed
// exponent bias that scales the colour on its way out of EDRAM -- this title
// resolves its HDR scene with bias -3, so a plain copy leaves the texture eight
// times brighter than the tonemap that samples it expects (catalog #33) -- and
// copy_dest_swap, which exchanges red and blue.
//
// The shader is deliberately parameterless in its SPIR-V: rectangle, offsets,
// scale and swap all arrive in push constants (ResolvePushConstants below), so
// one pipeline serves every resolve in a frame.
struct ResolvePushConstants
{
    int32_t srcOffset[2]; // @0,  in SOURCE units: samples
    int32_t dstOffset[2]; // @8,  in destination pixels
    int32_t extent[2];    // @16, the DESTINATION extent, in pixels
    float scale;          // @24, 2^copy_dest_exp_bias
    uint32_t swapRB;      // @28, copy_dest_swap
    // --- EDRAM is addressed in SAMPLES, the copy's rectangle in PIXELS ------
    // The destination steps one pixel per invocation; the source steps this
    // many samples, which is the surface's own msaa scale (1X 1,1; 2X 1,2;
    // 4X 2,2). Both 1 leaves the copy exactly as it was.
    int32_t srcScale[2]; // @32
    // Which sample within that pixel the copy starts at
    // (RB_COPY_CONTROL.copy_sample_select).
    int32_t sampleOffset[2]; // @40
    // The span the copy averages over, as an offset from the first sample:
    // (0,0) for a single-sample pick, (0,1) for the vertical pair a 2X k01
    // resolve averages, (1,1) for a 4X k0123. The shader always reads FOUR taps
    // -- c, c+dx, c+dy, c+dx+dy -- and multiplies the sum by tapWeight, which
    // is therefore always 0.25: with a zero delta all four are the same texel
    // and 4x * 0.25 is exactly x, with one axis set the pair appears twice and
    // 2(a+b) * 0.25 is exactly (a+b)/2. So one code path serves every selector
    // and a single-sample copy is bit-for-bit what it was.
    int32_t tapDelta[2]; // @48
    float tapWeight;     // @56, always 0.25 -- see tapDelta
};
bool BuildResolveComputeShader(std::vector<uint32_t> &spirv);

// EDRAM format reinterpretation, laid out to match the push-constant block in
// runtime/shaders/edram_reinterpret.comp exactly. The formats are the STORAGE
// forms (10 -> 2, 12 -> 3), because the _AS_ variants pack identically and a
// change between them is not a change.
struct ReinterpretPushConstants
{
    int32_t extent[2];
    uint32_t srcFormat;
    uint32_t dstFormat;
};

// The DEPTH resolve. The guest resolves depth to a k_8_8_8_8 destination -- the
// Xenos depth-as-colour resolve -- so this encodes our float32 depth back into
// the guest's 24-bit format (float24 20e4 for kD24FS8, unorm24 for kD24S8),
// packs it with stencil into a dword and writes the bytes as normalised
// components. `swapRB` in the push constants selects the format: 1 = float24.
bool BuildDepthResolveComputeShader(std::vector<uint32_t> &spirv, bool multisampled = false);

// EDRAM colour/depth aliasing, laid out to match the push-constant block in
// runtime/shaders/edram_depth_alias.comp exactly.
struct DepthAliasPushConstants
{
    int32_t extent[2];
    uint32_t isFloat24;
    uint32_t dstFormat;
};

// Whether this draw's primitive is POLYGONAL -- Xenia's
// draw_util::IsPrimitivePolygonal. Culling and faceness apply only to polygons;
// applying them to points or lines would drop geometry the hardware keeps.
bool IsPrimitivePolygonal(const uint32_t *registerFile);

// WHETHER THIS DRAW RASTERISES AT ALL, AND WHETHER IT NEEDS ITS PIXEL SHADER.
//
// Xenia's IssueDraw decides the fragment stage with THREE tests and this backend
// implemented only the middle one (edram_mode == kColorDepth). Measured against
// the console at the same guest frame, on the title screen: the same vertex
// shader 760aacf6212e632c splits 50 draws with a pixel shader and 5 without on
// our side, and 2 with / 53 without on the console's -- the same 55 draws
// classified oppositely, because 48 of them are a Z-PREPASS the console runs
// with no pixel shader at all.
//
//   rasterisationDone  -- draw_util::IsRasterizationPotentiallyDone. False means
//                         the draw covers nothing; Xenia SKIPS it outright
//                         (memexport aside) rather than issuing it.
//   pixelShaderNeeded  -- draw_util::IsPixelShaderNeededWithRasterization: false
//                         when the shader does not kill pixels, does not write
//                         depth, has no memory export, and every colour target
//                         it writes is fully masked out by RB_COLOR_MASK for
//                         that target's own component count. Only meaningful
//                         when rasterisationDone.
//
// Returns FALSE when it could not decide -- the pixel shader's microcode failed
// to analyse -- and the caller must then not read the fields. A classifier that
// answered "no fragment stage" on its own failure would silently turn every
// draw of a broken shader into a depth-only pass, which looks exactly like the
// console's own Z-prepass and would be invisible in every diagnostic here.
struct DrawClassification
{
    bool rasterisationDone = false;
    bool pixelShaderNeeded = false;
};
bool ClassifyDraw(const uint32_t *registerFile, const uint8_t *psUcode, size_t psSize,
                  uint64_t psHash, DrawClassification &out);

// Translates a single stage (vertex or pixel) under the given modification --
// the value DeriveShaderModifications produced for the draw's pair. Lets the
// whole-frame backend translate and cache each distinct (shader, modification)
// once.
bool TranslateShader(bool isVertex, const uint8_t *ucode, size_t size, uint64_t hash,
                     uint64_t modification, ShaderXlate &out);

// Derives the system-constants UBO (Xenia's SpirvShaderTranslator::
// SystemConstants) from our tracked register file, returned as raw bytes.
// Ports UpdateSystemConstantValues (non-FSI host-render-targets path).
void DeriveSystemConstants(const uint32_t *registerFile, bool applyTextureSigns,
                           std::vector<uint8_t> &out);

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
bool DeriveViewport(const uint32_t *registerFile, GuestViewport &out);

// Sampler state for one shader sampler binding, resolved against the texture
// fetch constant it names (Xenia texture_util::GetClampModesForDimension plus
// the kUseFetchConst filter fallbacks).
struct GuestSamplerState
{
    uint32_t magFilter = 0; // xenos::TextureFilter: 0 point, 1 linear
    uint32_t minFilter = 0;
    uint32_t mipFilter = 0;
    uint32_t clamp[3] = {0, 0, 0}; // xenos::ClampMode per axis
    uint32_t anisoMax = 0;         // 0 = anisotropy disabled, else max ratio
};

// Resolves one shader sampler binding against the fetch constant it names.
bool DeriveSamplerState(const uint32_t *fetch6, const ShaderSamplerBinding &sb,
                        GuestSamplerState &out);

} // namespace gears::draw
