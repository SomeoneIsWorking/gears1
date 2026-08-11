// Xenos register state -> Vulkan, and the guest colour formats. See
// gpu_draw_formats.h for what each of these is and why it is not a cosmetic
// choice; the comments are kept there so the interface carries the reasoning
// rather than the switch statements.

#include "gpu_draw_formats.h"

#include <cmath>

#include <lucent/config.h>

namespace gears::draw
{

VkPrimitiveTopology TopologyOf(uint32_t primType)
{
    switch (primType)
    {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 4: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    // A rectangle list has no Vulkan topology of its own: its three vertices go
    // in as a triangle list and the geometry shader emits the two-triangle strip
    // (see getRectGeomShader). Anything else unhandled also falls here.
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkBlendFactor BlendFactorOf(uint32_t f)
{
    switch (f)
    {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp BlendOpOf(uint32_t op)
{
    switch (op)
    {
    case 0: return VK_BLEND_OP_ADD;
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

VkCompareOp CompareOpOf(uint32_t f)
{
    switch (f & 7)
    {
    case 0: return VK_COMPARE_OP_NEVER;
    case 1: return VK_COMPARE_OP_LESS;
    case 2: return VK_COMPARE_OP_EQUAL;
    case 3: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 4: return VK_COMPARE_OP_GREATER;
    case 5: return VK_COMPARE_OP_NOT_EQUAL;
    case 6: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

GuestClamp GuestColorFormatClamp(uint32_t colorFormat)
{
    switch (colorFormat)
    {
    case 0:  // k_8_8_8_8
    case 1:  // k_8_8_8_8_GAMMA
    case 2:  // k_2_10_10_10
    case 10: // k_2_10_10_10_AS_10_10_10_10
        return GuestClamp::kRgba;
    case 3:  // k_2_10_10_10_FLOAT          (7e3 RGB, 2-bit UNORM alpha)
    case 12: // k_2_10_10_10_FLOAT_AS_16_16_16_16
        return GuestClamp::kAlphaOnly;
    default:
        return GuestClamp::kNone;
    }
}

// RB_COLOR_INFO.color_format (a xenos::ColorRenderTargetFormat) -> the host
// format the surface is rendered in. Ported from Xenia's
// VulkanRenderTargetCache::GetColorOwnDrawVulkanFormat.
//
// This is not a cosmetic choice. UE3 on the 360 renders its scene into a
// k_2_10_10_10_FLOAT (7e3) HDR surface whose values run to 32.0; rendering that
// into an 8888 UNORM host target clamps every highlight to 1.0 before the
// tonemap pass ever sees it, so the tonemap reads a flat white/black image.
VkFormat HostColorFormat(uint32_t colorFormat)
{
    switch (colorFormat)
    {
    case 0:  // k_8_8_8_8
    case 1:  // k_8_8_8_8_GAMMA -- gamma is applied on the way out, not stored
        return VK_FORMAT_R8G8B8A8_UNORM;
    case 2:  // k_2_10_10_10
    case 10: // k_2_10_10_10_AS_10_10_10_10
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 3:  // k_2_10_10_10_FLOAT (7e3, [0,32) RGB + unorm alpha)
    case 12: // k_2_10_10_10_FLOAT_AS_16_16_16_16
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 4:  // k_16_16 (fixed point -32..32)
        return VK_FORMAT_R16G16_SNORM;
    case 5:  // k_16_16_16_16 (fixed point -32..32)
        return VK_FORMAT_R16G16B16A16_SNORM;
    case 6:  // k_16_16_FLOAT
        return VK_FORMAT_R16G16_SFLOAT;
    case 7:  // k_16_16_16_16_FLOAT
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 14: // k_32_FLOAT
        return VK_FORMAT_R32_SFLOAT;
    case 15: // k_32_32_FLOAT
        return VK_FORMAT_R32G32_SFLOAT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

// Xenia's xenos::GetStorageColorFormat. The two _AS_ formats differ from their
// base only in BLENDING PRECISION -- the bits in EDRAM are packed identically --
// so anything asking "did the bit interpretation change" must compare storage
// formats, not raw ones. Comparing raw ones makes the frame's routine
// k_2_10_10_10_FLOAT <-> ..._AS_16_16_16_16 alternation look like 20 format
// changes a frame that are not changes at all.
uint32_t StorageColorFormat(uint32_t colorFormat)
{
    switch (colorFormat)
    {
    case 10: return 2;  // k_2_10_10_10_AS_10_10_10_10   -> k_2_10_10_10
    case 12: return 3;  // k_2_10_10_10_FLOAT_AS_16_16_16_16 -> k_2_10_10_10_FLOAT
    default: return colorFormat;
    }
}

const char* ColorFormatName(uint32_t f)
{
    switch (f)
    {
    case 0: return "k_8_8_8_8";
    case 1: return "k_8_8_8_8_GAMMA";
    case 2: return "k_2_10_10_10";
    case 3: return "k_2_10_10_10_FLOAT";
    case 4: return "k_16_16";
    case 5: return "k_16_16_16_16";
    case 6: return "k_16_16_FLOAT";
    case 7: return "k_16_16_16_16_FLOAT";
    case 10: return "k_2_10_10_10_AS_10_10_10_10";
    case 12: return "k_2_10_10_10_FLOAT_AS_16_16_16_16";
    case 14: return "k_32_FLOAT";
    case 15: return "k_32_32_FLOAT";
    default: return "?";
    }
}

// Bytes per pixel of a resolve DESTINATION format (RB_COPY_DEST_INFO's
// copy_dest_format, a xenos::ColorFormat). Needed to turn the byte distance
// between two RB_COPY_DEST_BASE values into a row offset. Returns 0 for a
// format this does not know, so the caller can report it rather than compute a
// nonsense offset from a guessed size.
uint32_t ColorFormatBytesPerPixel(uint32_t colorFormat)
{
    switch (colorFormat)
    {
    case 2: case 8: case 9:                       return 1; // k_8, k_8_A, k_8_B
    case 3: case 4: case 5: case 10: case 15:
    case 24: case 30:                             return 2; // 16-bit
    case 6: case 7: case 14: case 16: case 17:
    case 22: case 23: // k_24_8, k_24_8_FLOAT -- a DEPTH resolve destination
    case 25: case 31: case 36:                    return 4; // 32-bit
    case 26: case 32: case 37: case 50: case 54:
    case 55: case 56:                             return 8; // 64-bit
    case 38:                                      return 16;
    default:                                      return 0;
    }
}

// xenos::StencilOp -> Vulkan. The two enumerations happen to differ in the last
// four entries, so this is a table rather than a cast: getting it wrong silently
// inverts a shadow-volume count instead of failing.
VkStencilOp StencilOpOf(uint32_t op)
{
    switch (op & 7)
    {
    case 0:  return VK_STENCIL_OP_KEEP;
    case 1:  return VK_STENCIL_OP_ZERO;
    case 2:  return VK_STENCIL_OP_REPLACE;
    case 3:  return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 4:  return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 5:  return VK_STENCIL_OP_INVERT;
    case 6:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
}

// RB_DEPTH_INFO.depth_format is one bit at +16: 0 = kD24S8, 1 = kD24FS8. The
// texture formats they resolve to are k_24_8 (22) and k_24_8_FLOAT (23).
uint32_t DepthDestFormat(uint32_t rbDepthInfo)
{
    return ((rbDepthInfo >> 16) & 1) ? 23u : 22u;
}

// Xenia's ui::FloatToD3D11Fixed16p8, for the resolve rectangle's vertices.
//
// The tie-breaking differs from the D3D11 spec's round-to-nearest-even, and
// deliberately does not matter here: a resolve rectangle is written by the CPU
// with at most one fractional bit (the frame's are all x.5), so multiplying by
// 256 is exact and no rounding occurs at all. The early-exit clamps are Xenia's.
int32_t FloatToFixed16p8(float f)
{
    if (!(std::fabs(f) >= 1.0f / 512.0f))
        return 0;
    if (f >= 32768.0f - 1.0f / 256.0f)
        return (1 << 23) - 1;
    if (f <= -32768.0f)
        return -32768 * 256;
    return int32_t(std::lround(double(f) * 256.0));
}

// The host format one EDRAM base is given, from every RB_COLOR_INFO.color_format
// the frame renders it with. A base used with a single format keeps that
// format's own host equivalent; a base the frame reinterprets needs a container
// that holds all of them, and R16G16B16A16_SFLOAT holds every 8/10/16-bit
// colour format the Xenos can render (a k_8_8_8_8 value lands in [0,1] exactly,
// a 7e3 HDR value up to 32 and a k_16_16 fixed-point value up to 32 are all far
// inside float16's range).
VkFormat HostFormatFor(const std::set<uint32_t>& formats, bool& mixedOut)
{
    mixedOut = false;
    if (formats.empty())
        return VK_FORMAT_UNDEFINED;
    if (formats.size() == 1)
        return HostColorFormat(*formats.begin());
    mixedOut = true;
    // GEARS_DRAW_FORCE_LDR=1 -- CONTROL ARM ONLY, never a fix. Collapses a
    // reinterpreted surface to 8-bit UNORM to ask what the fixed-point render
    // target's source-colour clamp would have done. It destroys every HDR pass on
    // that surface, so it answers one question and breaks the frame.
    if (lucent::config::flag("DRAW_FORCE_LDR"))
        return VK_FORMAT_R8G8B8A8_UNORM;
    // A 32-bit float surface has no common container with a 4-channel one; the
    // caller reports that rather than this pretending otherwise.
    for (uint32_t f : formats)
        if (f == 14 || f == 15)
            return VK_FORMAT_R32G32_SFLOAT;
    return VK_FORMAT_R16G16B16A16_SFLOAT;
}

bool BlendIsIdentity(uint32_t blend0)
{
    const uint32_t cSrc = blend0 & 0x1F, cOp = (blend0 >> 5) & 0x7;
    const uint32_t cDst = (blend0 >> 8) & 0x1F;
    const uint32_t aSrc = (blend0 >> 16) & 0x1F, aOp = (blend0 >> 21) & 0x7;
    const uint32_t aDst = (blend0 >> 24) & 0x1F;
    return cSrc == 1 && cDst == 0 && cOp == 0 && aSrc == 1 && aDst == 0 && aOp == 0;
}

const char* PrimName(uint32_t primType)
{
    switch (primType)
    {
    case 1: return "point_list";
    case 2: return "line_list";
    case 3: return "line_strip";
    case 4: return "triangle_list";
    case 5: return "triangle_fan";
    case 6: return "triangle_strip";
    case 7: return "triangle_w_wflags";
    case 8: return "rectangle_list";
    case 12: return "line_loop";
    case 13: return "quad_list";
    case 14: return "quad_strip";
    case 15: return "polygon";
    default: return "other";
    }
}

} // namespace gears::draw
