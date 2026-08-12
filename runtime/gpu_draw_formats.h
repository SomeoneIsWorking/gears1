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

#include <lucent/config.h>

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

// The format with the same EDRAM PACKING, dropping the blending-precision
// modifier: the two _AS_ formats store exactly what their base format stores.
// "Has the bit interpretation of this surface changed" is a question about this
// value, never about the raw RB_COLOR_INFO field.
uint32_t StorageColorFormat(uint32_t colorFormat);

// Bytes per pixel of a TEXTURE format (xenos::TextureFormat), not a render
// target format -- the two enumerations do not agree and mixing them up reads a
// texture at the wrong stride.
uint32_t ColorFormatBytesPerPixel(uint32_t colorFormat);

// The destination format of a DEPTH resolve, from RB_DEPTH_INFO (0x2002).
//
// A depth copy's RB_COPY_DEST_INFO.copy_dest_format does not describe what is
// written: the hardware takes the format from RB_DEPTH_INFO.depth_format, and
// Xenia does the same (draw_util.cc GetResolveInfo overwrites copy_dest_format
// with DepthRenderTargetToTextureFormat(depth_format) when the source is
// depth). Measured on this title: the register says 6 (k_8_8_8_8) for every
// depth copy while the surfaces are really k_24_8_FLOAT and k_24_8.
//
// Returns a xenos::TextureFormat: 23 (k_24_8_FLOAT) for kD24FS8, else 22
// (k_24_8).
uint32_t DepthDestFormat(uint32_t rbDepthInfo);

// RB_DEPTHCONTROL's stencil ops (xenos::StencilOp) -> Vulkan.
VkStencilOp StencilOpOf(uint32_t op);

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

// EDRAM IS ADDRESSED IN SAMPLES, NOT PIXELS.
//
// An EDRAM tile is 80x16 SAMPLES (xenos.h), and RB_SURFACE_INFO.msaa_samples
// says how many samples a pixel of the surface occupies: 1X is 1x1, 2X adds a
// sample along Y, 4X along X and Y. So a 4X 640x360 surface and a 1X 1280x720
// one are the SAME 720 tiles, sample for sample -- which is how this title's
// shadow-mask fill covers the whole scene surface while submitting a quarter of
// the geometry, and why a renderer that maps guest pixels 1:1 to host pixels
// puts that fill in a corner (catalog #91).
//
// These two are the conversion. A draw's viewport and scissor are in PIXELS of
// its own surface; multiply by these and they are in samples, which is the one
// space every draw and every copy on a given EDRAM base agree in.
inline uint32_t MsaaScaleX(uint32_t msaaSamples)
{ return msaaSamples >= 2 /*k4X*/ ? 2u : 1u; }
inline uint32_t MsaaScaleY(uint32_t msaaSamples)
{ return msaaSamples >= 1 /*k2X*/ ? 2u : 1u; }

// HOW A COPY'S PIXEL RECTANGLE LANDS ON ITS SOURCE'S SAMPLES.
//
// Ported from Xenia's own EDRAM addressing (src/xenia/gpu/shaders/edram.xesli,
// XeEdramOffsetBytes), which is the same source the tile geometry above comes
// from -- so this is read out rather than guessed:
//
//     rt_sample_index  = pixel_index << (msaa >= 4X, msaa >= 2X)
//     rt_sample_index += (sample_index >> (1, 0)) & 1
//
// The first line is MsaaScaleX/MsaaScaleY. The second is the sample's position
// inside the pixel: sample 0 -> (0,0), 1 -> (0,1), 2 -> (1,0), 3 -> (1,1).
// Which sample a copy starts at is RB_COPY_CONTROL.copy_sample_select, run
// through Xenia's SanitizeCopySampleSelect first (draw_util.cc): DEPTH CANNOT
// BE AVERAGED, a 2X source has no samples 2 or 3, and a 1X source has only
// sample 0.
struct ResolveSampling
{
    uint32_t scaleX = 1, scaleY = 1;    // pixel -> sample step
    int32_t offsetX = 0, offsetY = 0;   // the first sample, inside the pixel
    // The averaged span, as an offset from that first sample. (0,0) is a
    // single-sample pick; (0,1) a vertical pair; (1,1) all four.
    int32_t spanX = 0, spanY = 0;
};
ResolveSampling DeriveResolveSampling(uint32_t rbSurfaceInfo,
                                      uint32_t copySampleSelect, bool isDepth);

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
    // RB_STENCILREFMASK (0x210D) and its back-face twin (0x210C): the reference
    // value, the read mask and the write mask, 8 bits each. Part of the
    // pipeline key because two draws differing only in stencil reference are
    // two different pipelines.
    uint32_t stencilRefMask = 0;
    uint32_t stencilRefMaskBf = 0;
    bool polygonal = false;
    // PA_CL_CLIP_CNTL.clip_disable, as depthClampEnable on the rasterizer: the
    // guest wants no near/far clipping, and clamping is the host's way to say
    // that. Part of the pipeline key because it is baked into the pipeline.
    bool depthClamp = false;

    bool operator<(const OutputMergerState& o) const
    {
        return std::tie(colorMask, blend0, depthControl, suScModeCntl,
                        stencilRefMask, stencilRefMaskBf, polygonal, depthClamp) <
               std::tie(o.colorMask, o.blend0, o.depthControl, o.suScModeCntl,
                        o.stencilRefMask, o.stencilRefMaskBf, o.polygonal,
                        o.depthClamp);
    }
};

// ONE HOST DEPTH+STENCIL IMAGE PER RB_DEPTH_INFO.depth_base, which is how the
// console addresses depth: by an EDRAM base, exactly as it addresses colour.
// Sharing a single image lets the shadow atlas (base 0x5a0) scribble over the
// scene's depth AND stencil (base 0x0), which is catalog #91's named root cause.
//
// THIS IS ONE FUNCTION BECAUSE IT WAS TWO CONSTANTS AND THEY DIVERGED. The
// geometry path read the knob and the resolve decoder did not, so under the
// split every depth resolve read, and every resolve-borne depth clear landed
// on, the SCENE's image whatever base the copy named -- the shadow atlas
// resolved at 0.0209 against the console's 0.7079. Any new site that needs the
// depth model calls this rather than reading the environment again.
//
// DEFAULT ON, measured. tools/depth_arm_ab.sh replays one frozen camera through
// both models and a repeat of the first as a noise floor: two identical runs
// differ by 0.0002-0.0059 per pass, and ten passes clear that. Nine favour the
// split -- the shadow atlas depth resolve 0.3380 -> 0.9994, both HDR resolves,
// the scene colour, and the FRONT BUFFER 0.5274 -> 0.6348 against a temporal
// ceiling of 0.7478. One favours the shared image and is unexplained: the
// first shadow mask, 0.9474 -> 0.8087. That regression is open (#91); it is one
// pass against nine and it does not justify shipping the wrong memory model.
//
// GEARS_DRAW_SPLIT_DEPTH=0 restores the shared image as a control arm. Absent
// means ON, so the correct model is what runs unless someone asks otherwise.
inline bool SplitDepthEnabled()
{
    static const bool on = !lucent::config::present("DRAW_SPLIT_DEPTH")
                           || lucent::config::flag("DRAW_SPLIT_DEPTH");
    return on;
}

} // namespace gears::draw
