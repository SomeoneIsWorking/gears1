---
id: C011
kind: claim
status: holds
created: 2026-08-05
tags: gpu,colour,resolve
depends: runtime/gpu_draw_resolve_decode.cpp, runtime/gpu_draw.cpp
---

## Claim

The red/blue relationship between the presented frame and the front-buffer resolve target is the GUEST's doing, not a renderer defect

## Evidence

Both the 717->0xc7f9000 and 743->0x311000 resolves carry resolve_swap_rb=1 (new diag column); resolvesUnstorable is 0 so neither fell back to the swap-less blit; and GEARS_DRAW_PIXEL_TRACE=835,258 shows one of the title's own draws exchanging the channels mid-frame -- (0.296875, 0.296875, 0.2685547) after draw 702 becomes (0.2685547, 0.296875, 0.296875) after draw 703, same pixel, same surface 0x2d0

## What would falsify it

if draw 703's pixel shader turns out to be one WE mistranslate into a channel exchange the microcode does not ask for, then the exchange is ours after all and this claim inverts
