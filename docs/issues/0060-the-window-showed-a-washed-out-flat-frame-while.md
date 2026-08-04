---
id: 60
title: The window showed a washed-out flat frame while the renderer's own output of the same scene was correct: sRGB swapchain
status: resolved
symptom: In a windowed run the game looks flat grey, low contrast, dithered, with no shadow detail; headless captures of the same scene look correct
tags: gpu,present,vulkan,colour,srgb
created: 2026-08-05
updated: 2026-08-05
---

## Symptom

Two screenshots from a windowed run: a courtyard scene rendered flat grey-green
with heavy dithering and no shadow detail, and a second scene flat grey with a
solid black band, in both cases with the UI text rendering correctly.

## The comparison that named it

The same scene, same build, captured from the renderer's own readback
(`scratch/screenshots/renderer_output_same_scene.png` -- the "Jack, rip that
door!" courtyard): correct exposure and contrast, textured wall, Marcus's
silhouette in shadow, mean pixel 23.9 of 255 with 0.49% saturated and 37.9%
near-black.

So the renderer's output is right and the window's is wrong. The defect is
between them: the present path.

## Cause

`CreateSwapchain` preferred `B8G8R8A8_UNORM` but fell back to `formats[0]` when
that exact format/colour-space pair was absent, and `formats[0]` on this driver is
`B8G8R8A8_SRGB`. The drawn frame is `R8G8B8A8_UNORM` holding bytes the guest has
already tonemapped -- display-ready values. `vkCmdBlitImage` between formats
CONVERTS: those bytes get treated as linear and encoded to sRGB, which lifts every
mid-tone hard and flattens the contrast. Blacks stay black, which is why the dark
corners and the black band survive while everything else turns grey.

## Why every instrument in this repo missed it

The renderer's screenshots come from its own readback, taken BEFORE the blit.
`GEARS_DRAW_FRAME_REPORT_EVERY`, the frame capture, `frame_replay` -- all of them
show the frame as rendered, none as presented. `GEARS_PRESENT_DUMP` exists for
exactly this and needs a window, so it had never been run: this project's headless
discipline put the entire present path outside every measurement it takes.

## Fix

Any UNORM format, whatever its colour space; failing that any non-sRGB format; and
if a surface offers nothing else, a warning saying the window will look washed out
and that the frame itself is correct. The chosen format is logged either way -- it
never was.

### Note (2026-08-05)
2026-08-05, CORRECTION to the cause above, from the test written to pin it.

The old selection preferred B8G8R8A8_UNORM with SRGB_NONLINEAR and only fell back
to formats[0] when that pair was absent. Run against the old logic, the test case
"sRGB listed first, B8G8R8A8_UNORM second" PASSES -- the old code picked the UNORM.
So the fallback can only have bitten if this surface does not offer that pair,
which is unusual, and that has NOT been shown.

What is shown, and stands: the same scene renders correctly from the renderer's
readback and appeared washed out in the window, so the defect is between the two.
What is NOT shown: that an sRGB swapchain is what did it. That was stated as the
cause with more confidence than the evidence carried.

Other candidates in the same gap, all inside the blit or after it: a chosen format
whose COLOUR SPACE is linear (EXTENDED_SRGB_LINEAR) rather than SRGB_NONLINEAR,
which encodes just the same; the compositor applying an HDR or colour-managed
transform to the window; or the swapchain image being sampled from a different
alias than the one written.

The format the surface actually yields is now logged on every run, which settles
the first two possibilities in one line, and `./run.sh --present-dump N` captures
what reaches the window. Neither existed when the claim was made.
