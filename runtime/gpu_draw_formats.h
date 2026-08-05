#pragma once

// Xenos register state -> Vulkan, and the guest colour formats.
//
// Split out of gpu_draw.cpp, which had grown to 6500 lines around a single
// 5300-line function. These are pure functions of the guest's register values:
// they touch no Vulkan objects and no renderer state, so they belong in their
// own translation unit where they can be read (and one day tested) without the
// renderer around them.

#include <cstdint>
#include <set>
#include <tuple>

#include <vulkan/vulkan.h>

namespace gears::draw
{

// VGT_DRAW_INITIATOR prim_type -> topology. A rectangle list has no Vulkan
// topology of its own: its three vertices go in as a triangle list and the
// geometry shader emits the two-triangle strip. Anything else unhandled also
// falls through to a triangle list.
VkPrimitiveTopology TopologyOf(uint32_t primType);

// The output-merger state is per draw and lives in the register file the draw
// carries: RB_COLOR_MASK (0x2104), RB_BLENDCONTROL0 (0x2201) and RB_DEPTHCONTROL
// (0x2200). Ignoring it is not a cosmetic simplification: a scene frame issues
// depth-only passes with colour writes fully masked off, and rendering those
// with an unconditional RGBA write paints the frame black.
VkBlendFactor BlendFactorOf(uint32_t f);
VkBlendOp BlendOpOf(uint32_t op);
VkCompareOp CompareOpOf(uint32_t f);

// "src ONE, dst ZERO, add" on both colour and alpha is the blend identity, so
// blending is only switched on when the guest actually asked for it. Shared
// between the pipeline builder and the per-draw diagnostic table, so the table
// reports the state the pipeline was actually built with.
bool BlendIsIdentity(uint32_t blend0);

// What the console's render target clamps in a shader's colour output before
// blending -- the property a widened host format loses (catalog #68).
//
// The 7e3 formats are the case worth spelling out: RGB is a float running to 32,
// but ALPHA is a 2-bit UNORM, so the hardware clamps alpha and leaves colour
// alone. The 16-bit fixed formats are excluded entirely: their range is -32..32,
// not [0,1], so clamping them would be a new defect rather than a fix.
enum class GuestClamp { kNone, kRgba, kAlphaOnly };
GuestClamp GuestColorFormatClamp(uint32_t colorFormat);

// RB_COLOR_INFO.color_format (a xenos::ColorRenderTargetFormat) -> the host
// format the surface is rendered in. Ported from Xenia's
// VulkanRenderTargetCache::GetColorOwnDrawVulkanFormat.
//
// This is not a cosmetic choice. UE3 on the 360 renders its scene into a
// k_2_10_10_10_FLOAT (7e3) HDR surface whose values run to 32.0; rendering that
// into an 8888 UNORM host target clamps every highlight to 1.0 before the
// tonemap pass ever sees it, so the tonemap reads a flat white/black image.
VkFormat HostColorFormat(uint32_t colorFormat);

const char* ColorFormatName(uint32_t f);

// Bytes per pixel of a TEXTURE format (xenos::TextureFormat), not a render
// target format -- the two enumerations do not agree and mixing them up reads a
// texture at the wrong stride.
uint32_t ColorFormatBytesPerPixel(uint32_t colorFormat);

const char* PrimName(uint32_t primType);

// The host format one EDRAM base is given, from every RB_COLOR_INFO.color_format
// the frame renders it with. A base used with a single format keeps that
// format's own host equivalent; a base the frame reinterprets needs a container
// that holds all of them, and R16G16B16A16_SFLOAT holds every 8/10/16-bit
// colour format the Xenos can render (a k_8_8_8_8 value lands in [0,1] exactly,
// a 7e3 HDR value up to 32 and a k_16_16 fixed-point value up to 32 are all far
// inside float16's range). `mixedOut` says whether the frame reinterpreted the
// base at all, so the caller can report it rather than this pretending
// otherwise.
VkFormat HostFormatFor(const std::set<uint32_t>& formats, bool& mixedOut);

// Xenia's ui::FloatToD3D11Fixed16p8, for the resolve rectangle's vertices.
//
// The tie-breaking differs from the D3D11 spec's round-to-nearest-even, and
// deliberately does not matter here: a resolve rectangle is written by the CPU
// with at most one fractional bit (the frame's are all x.5), so multiplying by
// 256 is exact and no rounding occurs at all. The early-exit clamps are Xenia's.
int32_t FloatToFixed16p8(float f);

// The output-merger registers that select a pipeline, kept together so the
// pipeline cache is keyed on exactly the state the pipeline bakes in.
struct OutputMergerState
{
    uint32_t colorMask = 0;    // RB_COLOR_MASK
    uint32_t blend0 = 0;       // RB_BLENDCONTROL0
    uint32_t depthControl = 0; // RB_DEPTHCONTROL
    // PA_SU_SC_MODE_CNTL: cull_front (bit 0), cull_back (bit 1), and face
    // (bit 2, 0 = front is counter-clockwise). Culling applies only to POLYGONAL
    // primitives -- Xenia gates the whole block on that, and faceness is
    // meaningless for points and lines.
    uint32_t suScModeCntl = 0;
    bool polygonal = false;

    bool operator<(const OutputMergerState& o) const
    {
        return std::tie(colorMask, blend0, depthControl, suScModeCntl, polygonal) <
               std::tie(o.colorMask, o.blend0, o.depthControl, o.suScModeCntl, o.polygonal);
    }
};

} // namespace gears::draw
