#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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

// How much of the console's physical memory the translated shaders can FETCH
// through -- the whole 512 MiB window, because a vertex fetch constant may name
// any of it.
//
// It lives here, in one place, because it is a property of the RENDERER and not
// of a frame: a captured frame stores the value that was in effect when it was
// recorded, and captures outlive the value. One taken before this was raised from
// 64 MiB carries 64 MiB, and replaying it honoured that -- 606 of 722 draws
// fetched past the mirror, read zero, collapsed at clipping, and the replay
// produced a plausible but WRONG picture of a frame the live runtime renders
// correctly (catalog #57).
inline constexpr uint32_t kGuestPhysicalMirrorBytes = 0x20000000;
// Must match xe::gpu::RegisterFile::kRegisterCount. Kept at the runtime seam so
// the command processor and capture format share one snapshot-size contract
// without exposing Xenia implementation headers through gpu_draw.h.
inline constexpr size_t kGpuRegisterSnapshotDwords = 0x5003;

// ---------------------------------------------------------------------------
// Whole-frame rendering: every DRAW_INDX/_2 of one frame, in submission order,
// into a single persistent colour+depth target, then presented/screenshotted.
// Each draw carries its own bound VS+PS and the register-file state that was
// live when it executed (constants change between draws), so the backend
// translates+caches each distinct shader pair, fills that draw's UBOs from its
// own snapshot, and accumulates the geometry.
struct FrameDrawItem
{
    // The Xenia register file as it stood at THIS draw (0x5003 dwords).
    // Constants, fetch slots and the draw initiator all live here; each draw needs its own
    // because the command stream reprograms them between draws.
    // A SHARED snapshot, not a per-draw copy. Copying the command processor's
    // whole 0x8000-dword storage included 0x2FFD dwords no renderer consumer can
    // address: Xenia's RegisterFile ends at 0x5003. Heavy live frames change
    // state at nearly every draw, so that padding alone was hundreds of MiB per
    // frame. Consecutive unchanged draws still share one snapshot.
    std::shared_ptr<const std::vector<uint32_t>> registerFile;

    const uint32_t *registers() const
    {
        return registerFile && registerFile->size() >= kGpuRegisterSnapshotDwords
                   ? registerFile->data()
                   : nullptr;
    }

    const uint8_t *vsUcode = nullptr; // borrowed, stable for the call
    size_t vsUcodeSize = 0;
    uint64_t vsHash = 0;
    const uint8_t *psUcode = nullptr;
    size_t psUcodeSize = 0;
    uint64_t psHash = 0;

    uint32_t primType = 0;
    uint32_t indexCount = 0; // == vertex count for a non-indexed (auto) draw
    bool indexed = true;     // false: auto/sequential index (source_select kAutoIndex)
    bool indexIs32 = true;
    // VGT_DMA_SIZE.swap_mode. Index byte order is draw state, not a property
    // of the title or index width; dropping it regrouped valid vertices into
    // different triangles while leaving all primitive counts plausible.
    uint32_t indexEndian = 0;
    uint32_t indexGuestBase = 0;
};

struct FrameDrawInputs
{
    const uint8_t *guestBase = nullptr;    // gears::Memory().Base()
    uint32_t guestPhysicalMirrorBytes = 0; // low guest memory mirrored into the SSBO
    // How much physical RAM is mapped at the 0x0 alias (512 MiB on the
    // console). Texture fetch constants name addresses across the whole of it,
    // well past the SSBO mirror, so the texture decoder reads guestBase
    // directly and bounds-checks against this.
    uint32_t guestWindowBytes = 0;
    uint32_t width = 1280;
    uint32_t height = 720;
    std::vector<FrameDrawItem> draws; // in submission order
    // Whether this frame gets the full census: the per-pixel coverage scan, the
    // summary lines and the PPM screenshot. ~40 ms, which is most of a warm
    // frame -- it belongs to a capture, not to every frame of a live run.
    bool report = true;
    // An explicit HTTP readback. This is intentionally independent of report:
    // probing a frame must not emit capture artifacts or consume capture quota.
    bool probe = false;

    // THE FRONT BUFFER THE GUEST NAMED, from the swap packet that ended this frame
    // (VdSwap's data[0], physical, alias bits included). Zero when unknown.
    //
    // The renderer used to decide what to present by rule -- the surface the last
    // geometry draw wrote -- which is a guess that happens to be right on this
    // title's pipeline. The guest states the answer outright, and a frame where the
    // guess and the statement disagree is a frame presenting the wrong buffer: the
    // scene's linear-light HDR surface instead of the tonemapped one, say, which
    // looks like flat unlit grey with all the texture detail present.
    uint32_t frontBufferAddress = 0;
    // The front buffer's TEXTURE FETCH CONSTANT, the six dwords the guest hands
    // VdSwap in its Direct3D 9 texture header (host order). This is the only
    // statement of the front buffer's FORMAT, size, tiling and swizzle that the
    // guest makes -- the address alone does not say how to read the bytes.
    //
    // Captured because an oracle needs it: Xenia's swap path takes the front
    // buffer from fetch constant 0 (vulkan_texture_cache.cc RequestSwapTexture),
    // exactly as its kernel's VdSwap posts it, so a replayed frame with no fetch
    // constant produces no image at all. Deriving one from the address plus a
    // guessed format would make the oracle agree with our guess by construction.
    // All zero when the capture predates this or the guest passed none.
    uint32_t frontBufferFetch[6] = {};
    // The display gamma ramp the guest uploaded, 256 entries packed as the
    // hardware's DC_LUT_30_COLOR (blue 0..9, green 10..19, red 20..29), or null
    // when the title has programmed none. Scan-out puts every presented pixel
    // through it; this title's is markedly non-linear (it darkens the low end),
    // so a frame shown without it is brighter than the console's -- measured,
    // applying it took a gameplay frame from 95.1% of pixels above 8/255 to
    // 71.6%, against the reference renderer's 71.9-75.8% on the same walk. See
    // catalog #78 and runtime/vd_null_gpu.cpp for how it is accumulated.
    const uint32_t *gammaRamp = nullptr;
    // A monotonic frame index. When >= 0, a reported frame's screenshot is named
    // frame_<sequence>.ppm rather than overwriting frame.ppm, so a menu walk
    // leaves a filmstrip instead of only its last frame.
    long sequence = -1;
};

// Renders every draw of the frame into one persistent target and writes a PPM
// screenshot (scratch/screenshots/frame.ppm, or GEARS_DRAW_DIR/frame.ppm).
// Reports per-frame: draws issued vs total, distinct shader pairs, and any draw
// it could not issue with the reason. Returns true if the frame rendered.
bool RenderFrame(const FrameDrawInputs &in);

// Submits a live frame without making the render thread wait for the GPU. The
// completion runs after the frame's fence and all resources it references have
// retired. Diagnostic frames transparently take the synchronous path and still
// obey the same completion contract.
using FrameRenderCompletion = std::function<void(bool)>;
bool SubmitFrameRender(const FrameDrawInputs &in, FrameRenderCompletion &&completion);

// Waits for every frame accepted by SubmitFrameRender. Teardown, capture, and
// renderer-reset boundaries use this; ordinary live frame progress does not.
bool WaitForRendererGpuIdle();

// Drops everything the renderer keeps between frames -- surfaces, pipelines,
// shader translations, caches -- so the next RenderFrame rebuilds from nothing.
//
// The interleaved comparer needs this and nothing else does: the two arms must
// each be a CLEAN render, and almost every knob worth comparing is consumed
// while building persistent state. A surface's host format is chosen once, when
// the surface is created, so arm B silently inherits arm A's surfaces and the
// knob appears to change nothing -- which reads exactly like "the knob does not
// matter".
void ResetRendererForComparison();

// The last frame RenderFrame read back, as tightly-packed R8G8B8A8 rows,
// 1280x720, or empty if none. The presenter uploads this into the swapchain
// when it cannot blit the drawn image directly (the two sides ended up on
// different devices); it is also what the census and the PPM screenshot read.
const std::vector<uint8_t> &GuestFramePixels();
uint32_t GuestFrameWidth();
uint32_t GuestFrameHeight();

} // namespace gears
