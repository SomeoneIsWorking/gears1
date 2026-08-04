#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// The guest-draw graphics backend. Separate from gpu_present.cpp (which owns
// the window and swapchain) and from vd_null_gpu.cpp (the command processor /
// protocol): this file owns the guest-draw graphics pipeline and nothing else.
//
// It runs headless -- it will create its own Vulkan instance/device when there
// is no presenter to share one with -- so the measurement harnesses that have
// no display still exercise and verify the draw. When the presenter IS up, both
// sides share one device and the rendered image is handed over as an image
// (gpu_shared_device.h) rather than through host memory.
namespace gears
{

// ---------------------------------------------------------------------------
// Whole-frame rendering: every DRAW_INDX/_2 of one frame, in submission order,
// into a single persistent colour+depth target, then presented/screenshotted.
// Each draw carries its own bound VS+PS and the register-file state that was
// live when it executed (constants change between draws), so the backend
// translates+caches each distinct shader pair, fills that draw's UBOs from its
// own snapshot, and accumulates the geometry.
struct FrameDrawItem
{
    // The register file as it stood at THIS draw (0x8000 dwords). Constants,
    // fetch slots and the draw initiator all live here; each draw needs its own
    // because the command stream reprograms them between draws.
    // A SHARED snapshot, not a per-draw copy. The register file is 32768
    // dwords; copying it per draw cost ~90 MB of memcpy per gameplay frame
    // (~700 draws x 128 KB) and was the single largest cost in recording a
    // frame. Consecutive draws overwhelmingly share identical register state,
    // so the producer snapshots only when a register has actually changed and
    // every draw in between points at the same one.
    std::shared_ptr<const std::vector<uint32_t>> registerFile;

    const uint32_t* registers() const
    {
        return registerFile && registerFile->size() >= 0x8000 ? registerFile->data()
                                                             : nullptr;
    }

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

// The last frame RenderFrame read back, as tightly-packed R8G8B8A8 rows,
// 1280x720, or empty if none. The presenter uploads this into the swapchain
// when it cannot blit the drawn image directly (the two sides ended up on
// different devices); it is also what the census and the PPM screenshot read.
const std::vector<uint8_t>& GuestFramePixels();
uint32_t GuestFrameWidth();
uint32_t GuestFrameHeight();

} // namespace gears
