---
id: C011
kind: claim
status: falsified
created: 2026-08-05
tags: gpu,colour,resolve
depends: runtime/gpu_draw_resolve_decode.cpp, runtime/gpu_draw.cpp
falsified_on: 2026-08-06
---

## Claim

The red/blue relationship between the presented frame and the front-buffer resolve target is the GUEST's doing, not a renderer defect

## Evidence

Both the 717->0xc7f9000 and 743->0x311000 resolves carry resolve_swap_rb=1 (new diag column); resolvesUnstorable is 0 so neither fell back to the swap-less blit; and GEARS_DRAW_PIXEL_TRACE=835,258 shows one of the title's own draws exchanging the channels mid-frame -- (0.296875, 0.296875, 0.2685547) after draw 702 becomes (0.2685547, 0.296875, 0.296875) after draw 703, same pixel, same surface 0x2d0

## What would falsify it

if draw 703's pixel shader turns out to be one WE mistranslate into a channel exchange the microcode does not ask for, then the exchange is ours after all and this claim inverts

## FALSIFIED 2026-08-06

The Xenia oracle, booted from the extracted tree, renders the SAME STATIC MAIN MENU red (R/G 5.13) where we render it blue (R/G 0.83) -- a screen that does not move, so no moment caveat applies. GEARS_DRAW_PIXEL_TRACE on scratch/frames/menu.gfr then localises the exchange to ONE draw: after draw 76 a background pixel is (0.1121, 0.0301, 0.0145) and after draw 78 (ps 0x629226076307234e, the final full-screen composite) it is (0.0145, 0.0301, 0.1121) -- same values, R and B exchanged. Confirmed on a second pixel 560 px away. This claim's own falsifier named this case: the exchange happens at a draw whose output the reference says should be red, so it is not the title's own doing and it is not defect-free. What remains open is WHERE in that draw (microcode routing, the fetch swizzle on the resolve target it samples, or its export order) -- ucode_reduce.py has not been run on it yet, so 'we mistranslate the microcode' is the leading candidate rather than an established fact. Details: catalog #62.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
