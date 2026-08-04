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
