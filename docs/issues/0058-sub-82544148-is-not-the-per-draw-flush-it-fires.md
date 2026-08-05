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

### Note (2026-08-05)
2026-08-05. The per-draw emitter is NOT among the eleven functions this seam has
probed. Rates over a 2867-frame run (scripted walk, menus + Act 1 mixed), taken
from the per-swap census:

  82220858 SetTexture   74.0 /frame
  822212D8 ring kick     9.2 /frame
  822218C0 submit        6.3 /frame
  82221980 flush         3.9 /frame
  82221A68 ticket wait   1.3 /frame
  8223B200 CPU list      1.8 /frame
  8223E3E0 Present       1.0 /frame
  8223E860 present pump  1.0 /frame
  82544148 "draw flush"  0.9 /frame   <- confirms one per frame
  8223BA18 frame block   0.9 /frame
  8221D3A8 movie draw    0.3 /frame

The frames in that run carry 170 draws (menus) to 744 (gameplay). Nothing probed
is within two orders of magnitude of either, so the function that emits DRAW_INDX
has never been instrumented -- guessing addresses from the static map has now been
tried and did not find it.

METHOD FOR NEXT TIME, since address-guessing is exhausted: catch the writer. The
guest writes the DRAW_INDX packet words into the ring, and hle_d3d.cpp already has
the machinery to catch that -- WatchArm/WatchProtect mprotects a guest page and
takes the faulting context, which is how the worker's queue head was pinned down.
Point it at the ring pages where the command processor sees DRAW_INDX arrive, read
the LR out of the faulting context, and the emitter names itself. That is a
measurement, not a guess, and it works no matter how the draw is dispatched.

### Note (2026-08-05)
### Note (2026-08-05, later): the emitter is probably not worth finding

Before spending a third session on the ring-page watch, someone should ask what
having the emitter would buy. Working through it:

- the DRAW ITSELF -- primitive type, index count, buffers, whole register state --
  already arrives intact in the PM4 stream;
- WHICH UE3 PASS a draw belongs to is now recovered from that same stream by
  `tools/pass_structure.py`, with no emitter;
- WHICH MATERIAL it is, is the pixel-shader hash the renderer already keys on;
- TEXTURE SLOT BINDINGS are already cross-checked from the seam via the wrapped
  SetTexture (the 122-bases agreement above).

A static shot was also tried and missed: the recompiled corpus (192 TUs, 177 MB)
contains no PM4 TYPE3 header constant carrying opcode 0x22 (DRAW_INDX) or 0x36
(DRAW_INDX_2), so the packet header is assembled at runtime from a variable count
and cannot be found by grepping for the opcode.

What the emitter WOULD carry that PM4 does not is mesh and material identity at
the engine level. That is real but narrow, and nothing on the native-renderer
roadmap currently depends on it -- the EDRAM-tiling collapse did not need it, and
neither would render-target ownership or resolution scaling. The ring-watch method
above is still the right method IF the emitter is wanted; the point of this note is
that it should stop being described as the blocker.
