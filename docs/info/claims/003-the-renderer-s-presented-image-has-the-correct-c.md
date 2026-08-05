---
id: C003
kind: claim
status: holds
created: 2026-08-05
tags: gpu,present,colour
depends: runtime/gpu_draw.cpp
---

## Claim

The renderer's presented image has the correct channel order; the guest's copy red/blue swap is cancelled by the scanout format and must not be undone

## Evidence

On the boot movie frame the source surface holds the Epic Games logo in its correct ORANGE and the front-buffer resolve holds it in BLUE (scratch/ab/boot_compare.png). Cross-checked against in-engine Gears footage (Wikipedia 'Mad World' still): reference normalised R0.900 G1.087 B1.013 -- red lowest; our frame R0.796 G1.096 B1.107 same ordering; R/B-swapped variant R1.107 G1.096 B0.796, inverted. catalog #64

## What would falsify it

a frame whose front-buffer resolve sets copy_dest_swap=0 while another sets 1, presented through the same path and both correct -- that would mean the compensation is not uniform. Also: this rests on TWO frames (boot movie, Act 1); it is not a survey of the whole game
