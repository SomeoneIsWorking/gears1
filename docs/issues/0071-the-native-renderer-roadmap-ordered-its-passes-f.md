---
id: 71
title: The native-renderer roadmap ordered its passes from UE3's source instead of the title's frames, and the next pass did not exist
status: resolved
symptom: a declared native pass (height fog) has no pixel-shader hash and no draw to A/B against; searching for it finds nothing and it is not obvious whether the search or the frame is at fault
tags: native-renderer,ue3,gpu,shaders,method
created: 2026-08-05
updated: 2026-08-05
---

## Symptom

`runtime/native_pass.cpp` declared a height-fog pass with `pixelShaderHash = 0`
and `docs/native-renderer.md` put it next in the order of work, ahead of the base
pass, on the grounds that its maths fits on a page. Nobody had checked that the
title emits it.

## What was measured

UE3's `FSceneRenderer::RenderFog` (`FogRendering.cpp:614`) is the only place in
the engine that calls `RHISetColorWriteMask(CW_RED|CW_GREEN|CW_BLUE)`. It draws
2 primitives, depth test on, `BO_Add / BF_One / BF_SourceAlpha`. That is a
signature nothing else in a UE3 frame has.

Scanned four captures — `act1_v2` (157 draws), `courtyard` (726), `bright` (826),
`play_v2` (849), 2,558 draws total — through `GEARS_DRAW_DIAG`. **Four** draws
have colour mask 7, one per capture, all the same pixel shader
`0x629226076307234e`, all with **blending off**. Its microcode is a depth fetch,
`ConvertFromDeviceZ` (`mad c0.z, -c0.w; rcp`), a 4x4 reprojection, a perspective
divide, a clamped screen-space velocity and a sampling loop — `RenderVelocities`,
motion blur. Zero draws match `RenderFog`.

Cross-checks from the sources: `BasePassCommon.usf` gates vertex fog on
`NEEDS_BASEPASS_FOGGING` = translucent/additive/modulate blending only, so opaque
geometry carries no fog either. Both of UE3's fog producers are absent, which is
consistent rather than contradictory.

## Resolution

The roster entry is **withdrawn**, not written. Writing it would have meant a
shader with an invented hash against a draw that does not exist and nothing to
A/B it against — the RE sin this project tracks.

The real fix is upstream of the fog: **step 3 (recover the pass structure) should
have come before step 4 (write the next pass)**, because step 3 is what says which
passes the game actually runs. `tools/pass_structure.py` now does step 3, and
`GEARS_DRAW_DIAG` emits a row per resolve so the pass boundaries are visible at
all — they used to be dropped, which is why the frame read as a flat draw stream
with no seams. Claim C004, instrument I003.

## What NOT to re-derive

- Do not go looking for the fog pass again without new captures. The negative is
  quantified: 2,558 draws, 4 candidates by colour mask, all four identified.
- `0x629226076307234e` is motion blur. `0x9610bf8038af9aaf` is the uber
  post-process blend (depth-of-field blend against a blurred target, then
  shadows/highlights/midtones colour grading, then a final gamma) — decoded from
  its microcode, not yet written natively.
- `0x63c971f5e9d59913` is the title's pass-through pixel shader
  (`alloc colors; exece; max oC0, r0, r0`), bound for EDRAM clears and for the
  depth-only prepass. It is the discriminator the pass classifier keys on.
