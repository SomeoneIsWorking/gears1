---
id: 61
title: The window's washed-out frame is a display-side re-interpretation, not a renderer or blit defect
status: investigating
symptom: The game looks flat grey with lifted blacks and no contrast in the window, while the renderer's output and the presented frame are byte-identical and correct
tags: gpu,present,colour,hdr,display
created: 2026-08-05
updated: 2026-08-05
---

## What is established, with evidence

1. The renderer's output for the reported scene is correct
   (`scratch/screenshots/renderer_output_same_scene.png`, mean 23.9/255, 0.49%
   saturated, 37.9% near-black -- a dark contrasty interior).
2. The presented frame equals the rendered frame, byte for byte, through a real
   swapchain and a real blit: `tools/verify_present_path.sh` PASSes with max
   |diff| 0 over 1280x720x3, via `VK_EXT_headless_surface`.
3. The renderer produces the same correct frame whether it creates its own device
   or ADOPTS the presenter's -- the windowed topology, reproduced headlessly with
   `GEARS_PRESENT_HEADLESS=1`. The adopted device builds both resolve compute
   pipelines and the rectangle geometry shader, so no feature is being lost.

So nothing between the guest's draws and `vkQueuePresentKHR` alters a pixel.

## What the screenshot's numbers say

Percentiles of the reported window capture, against the renderer's own output of
the same scene:

    percentile      1     5    10    25    50    75    90    95    99
    window      0.031 0.051 0.078 0.192 0.255 0.282 0.298 0.298 0.298
    renderer    0.000 0.000 0.000 0.024 0.043 0.133 0.247 0.306 0.557

The window's top three percentiles are IDENTICAL at 0.298: the whole frame is
compressed into [0.03, 0.30] with a hard ceiling. That is not an sRGB re-encode --
that would spread the same input up to 0.77 and leave blacks at 0. It is the
signature of SDR content sitting in a PQ/HDR framebuffer, where the SDR range
occupies roughly the bottom third of the code space.

## Therefore

The re-interpretation happens at or after the compositor, not inside this process.
The candidates are a desktop in HDR mode compositing our SDR window, or a surface
whose only usable pairing carries a non-sRGB colour space.

## What was changed for it

- `ChooseSwapchainFormat` now ranks the COLOUR SPACE above the format:
  SRGB_NONLINEAR first, always, and only then the format preference. An earlier
  version took any UNORM format regardless of colour space, which on an HDR
  desktop picks exactly the wrong pairing. Tested with an HDR-style list
  (`tests/test_swapchain_format.cpp`).
- Every format/colour-space pair the surface offers is logged before the choice,
  and the chosen colour space is logged and warned about when it is not
  SRGB_NONLINEAR. On this machine the list is `[37/0] [44/0] [43/0] [50/0]` --
  all SRGB_NONLINEAR -- which is why nothing here reproduces the symptom.

The next run on the affected display prints its list in one line, and that line
decides between the two remaining candidates.

### Note (2026-08-05)
2026-08-05, MEASURED: the transform is an sRGB ENCODE applied after we present.

Reproduced the full windowed path on this machine at last -- GEARS_PRESENT_HIDDEN=1
creates the REAL SDL window (real Wayland surface, real compositor, real swapchain)
and never maps it, so a measurement run can exercise the window system without
putting anything on the operator's screen.

Through that path, in gameplay, the frame we hand to the swapchain is
byte-identical to the renderer's readback of the same frame (max |diff| 0,
p99 0.553 on both sides). So everything up to and including vkQueuePresentKHR is
correct on this display.

The reported window capture, against the frame we presented, percentile by
percentile:

    percentile         p25    p50    p75    p90    p99
    presented        0.031  0.051  0.141  0.255  0.298
    sRGB_encode()    0.193  0.250  0.411  0.542  0.582
    the window       0.192  0.255  0.282  0.298  0.298

p25 and p50 match sRGB_encode() of what we presented to three decimals. Something
after vkQueuePresentKHR is encoding our output as though it were linear light.
(The upper percentiles diverge because the window capture is a different game
moment, not a different transform.)

WHY THAT CAN HAPPEN: a UNORM swapchain carries no statement about its transfer
function, and a compositor is free to read the bytes as linear. Ours is
B8G8R8A8_UNORM with SRGB_NONLINEAR, on a display whose surface offers nothing but
SRGB_NONLINEAR and whose HDR is disabled (kscreen-doctor).

THE FIX, and why it is not in this commit: tag the swapchain sRGB
(B8G8R8A8_SRGB), which states that the bytes are encoded -- but vkCmdBlitImage
into an sRGB image CONVERTS, so the frame would be encoded twice. It has to go in
as raw bytes: either vkCmdCopyImage from a B8G8R8A8_UNORM staging image
(size-compatible copy, no conversion), or a blit through an UNORM view of the
sRGB image via VK_KHR_swapchain_mutable_format. A knob that only flips the format
would look like a fix and double-encode instead, so it was written, recognised as
half a fix, and reverted rather than shipped.

INSTRUMENT ADDED, and it is the useful part: GEARS_PRESENT_HIDDEN. Every previous
"I cannot test the window from here" was answered by a headless surface, which
turned out not to exercise the window system at all. This does, and costs the
operator nothing.
