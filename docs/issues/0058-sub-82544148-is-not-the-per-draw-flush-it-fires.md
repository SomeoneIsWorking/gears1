---
id: 58
title: sub_82544148 is not the per-draw flush: it fires once per frame against 744 draws
status: resolved
symptom: The D3D seam map labels 0x82544148 'THE draw flush (state -> TYPE0 packets + draw)', but wrapping it counts one call per frame
tags: hle,d3d,re,seam,instrument
created: 2026-08-05
updated: 2026-08-05
---

## What was believed

`docs/d3d-seam.md` listed 0x82544148 as "THE draw flush (state -> TYPE0 packets +
draw); the largest function in the render path (~21k lines of recompiled code)",
with "~0 in movie phase" as its measured rate. The identification came from static
reading; the only rate ever measured for it was in a phase where it barely runs.

## What it measures now

Wrapped at the seam (`runtime/hle_d3d.cpp`, super-call, guest body unchanged) and
counted against the renderer's own per-frame draw count, on a scripted walk into
Act 1:

- menus: 1 call per frame, frames of ~170 draws
- gameplay: 1 call per frame, frames of 744 draws

Exactly one, in both phases. Whatever 0x82544148 is, it is not what emits the
frame's draws -- it runs once for a frame that contains hundreds of them. The
"state flush" half of the label may well be right; the "+ draw" half cannot be.

## Why the static reading was believable

It is the largest function in the render path and it does write TYPE0 register
packets, so reading it bottom-up gives every impression of a draw emitter. Nothing
about the code says how OFTEN it runs, and the one rate that had been measured
(~0/swap) was taken in the movie phase, which draws through a different path
entirely (0x8221D3A8) and therefore could not have contradicted it.

## What agreed

The same wrap also cross-checks textures, and there the map holds up. Per frame,
the title's own SetTexture calls bind 122 distinct texture base addresses, and the
renderer's PM4-derived census reports 122 distinct bases for that frame -- the
same number, from opposite ends. In the movie phase both sides report the same
three planes. So the register-file inference for textures is sound; it is the
draw-emission story that is wrong.

## Next

Find what actually emits the frame's draws. It runs ~744 times a frame in
gameplay and ~170 in menus, so a per-frame count is enough to identify it: probe
the candidates in the RHI/draw zone and look for one whose rate tracks the
renderer's draw count. 0x8221D3A8 (the movie path's draw) is the first to try,
since the movie phase draws through it.
