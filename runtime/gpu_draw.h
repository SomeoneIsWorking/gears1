#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The first real guest draw: the hot vertex/pixel pair's full-screen quad,
// rendered with its translated SPIR-V into an offscreen colour target and read
// back to a screenshot. Separate from gpu_present.cpp (which owns the window
// and swapchain) and from vd_null_gpu.cpp (the command processor / protocol):
// this file owns the guest-draw graphics pipeline and nothing else.
//
// It runs headless -- its own Vulkan instance/device, no surface -- so the
// measurement harnesses that have no display still exercise and verify the
// draw. When the presenter is up, the rendered pixels are handed to it for
// upload into the swapchain (GuestFramePixels below), so the same real frame
// appears in the window.
namespace gears
{

// One captured hot-pair DRAW_INDX, everything the pipeline needs. Pointers are
// borrowed for the duration of the call only; the command processor thread
// blocks while RenderHotDraw runs, so the register file and guest memory are
// stable.
struct HotDrawInputs
{
    const uint32_t* registerFile = nullptr; // g_gpuRegisters.data(), 0x8000 dwords
    const uint8_t* guestBase = nullptr;      // gears::Memory().Base()
    uint32_t guestPhysicalMirrorBytes = 0;   // how much low guest memory to mirror into the SSBO

    const uint8_t* vsUcode = nullptr;        // vertex microcode, big-endian (as the GPU reads it)
    size_t vsUcodeSize = 0;
    const uint8_t* psUcode = nullptr;        // pixel microcode, big-endian
    size_t psUcodeSize = 0;
    uint64_t vsHash = 0;
    uint64_t psHash = 0;

    uint32_t primType = 0;                   // VGT_DRAW_INITIATOR prim_type
    uint32_t indexCount = 0;
    bool indexIs32 = true;
    uint32_t indexGuestBase = 0;
    uint32_t indexSwap = 0;                  // VGT_DMA_SIZE swap_mode (endian)
};

// Builds the pipeline from the two SPIR-V modules, fills the UBOs/SSBO from the
// register file, issues the indexed draw into an offscreen 1280x720 target,
// reads it back and writes a PPM screenshot under scratch/screenshots/. Returns
// true if a frame was rendered and the screenshot written. Safe to call when no
// Vulkan device is available (returns false, reports why). One-shot use is the
// caller's responsibility.
bool RenderHotDraw(const HotDrawInputs& in);

// ---------------------------------------------------------------------------
// Whole-frame rendering: every DRAW_INDX/_2 of one frame, in submission order,
// into a single persistent colour+depth target, then presented/screenshotted.
// This generalises RenderHotDraw beyond the hot pair -- each draw carries its
// own bound VS+PS and the register-file state that was live when it executed
// (constants change between draws), so the backend translates+caches each
// distinct shader pair, fills that draw's UBOs from its own snapshot, and
// accumulates the geometry.
struct FrameDrawItem
{
    // The register file as it stood at THIS draw (0x8000 dwords). Constants,
    // fetch slots and the draw initiator all live here; each draw needs its own
    // because the command stream reprograms them between draws.
    std::vector<uint32_t> registerFile;

    const uint8_t* vsUcode = nullptr; // borrowed, stable for the call
    size_t vsUcodeSize = 0;
    uint64_t vsHash = 0;
    const uint8_t* psUcode = nullptr;
    size_t psUcodeSize = 0;
    uint64_t psHash = 0;

    uint32_t primType = 0;
    uint32_t indexCount = 0;   // == vertex count for a non-indexed (auto) draw
    bool indexed = true;       // false: auto/sequential index (source_select kAutoIndex)
    bool indexIs32 = true;
    uint32_t indexGuestBase = 0;
};

struct FrameDrawInputs
{
    const uint8_t* guestBase = nullptr;    // gears::Memory().Base()
    uint32_t guestPhysicalMirrorBytes = 0; // low guest memory mirrored into the SSBO
    // How much physical RAM is mapped at the 0x0 alias (512 MiB on the
    // console). Texture fetch constants name addresses across the whole of it,
    // well past the SSBO mirror, so the texture decoder reads guestBase
    // directly and bounds-checks against this.
    uint32_t guestWindowBytes = 0;
    uint32_t width = 1280;
    uint32_t height = 720;
    std::vector<FrameDrawItem> draws;      // in submission order
    // Whether this frame gets the full census: the per-pixel coverage scan, the
    // summary lines and the PPM screenshot. ~40 ms, which is most of a warm
    // frame -- it belongs to a capture, not to every frame of a live run.
    bool report = true;
    // A monotonic frame index. When >= 0, a reported frame's screenshot is named
    // frame_<sequence>.ppm rather than overwriting frame.ppm, so a menu walk
    // leaves a filmstrip instead of only its last frame.
    long sequence = -1;
};

// Renders every draw of the frame into one persistent target and writes a PPM
// screenshot (scratch/screenshots/frame.ppm, or GEARS_DRAW_DIR/frame.ppm).
// Reports per-frame: draws issued vs total, distinct shader pairs, and any draw
// it could not issue with the reason. Returns true if the frame rendered.
bool RenderFrame(const FrameDrawInputs& in);

// The last frame RenderHotDraw produced, as tightly-packed R8G8B8A8 rows,
// 1280x720, or empty if none. The presenter uploads this into the swapchain so
// the real guest frame shows in the window instead of the synthetic clear.
const std::vector<uint8_t>& GuestFramePixels();
uint32_t GuestFrameWidth();
uint32_t GuestFrameHeight();

} // namespace gears
