---
id: C004
kind: claim
status: holds
created: 2026-08-05
tags: gpu,native-renderer,ue3
depends: tools/pass_structure.py
---

## Claim

Gears of War 1 emits NO UE3 RenderFog pass in any frame this project has captured; the roster's height-fog entry was written against UE3's source, not against the title's frames

## Evidence

UE3 FSceneRenderer::RenderFog (FogRendering.cpp:614) is the only place in the engine that sets RHISetColorWriteMask(CW_RED|CW_GREEN|CW_BLUE), and it draws 2 primitives with depth test on and BO_Add/BF_One/BF_SourceAlpha blending. Scanned all four captures (act1_v2 157 draws, courtyard 726, bright 826, play_v2 849 = 2558 draws) via GEARS_DRAW_DIAG: exactly 4 draws have colour mask 7, one per capture, all the same pixel shader 0x629226076307234e, and all with blending OFF. That shader's microcode is a depth fetch, a 4x4 reprojection, a perspective divide, a clamped screen-space velocity and a sampling loop -- RenderVelocities/motion blur, not fog. Zero draws match RenderFog. Consistent with BasePassCommon.usf gating vertex fog on NEEDS_BASEPASS_FOGGING (translucent/additive/modulate blending only), so opaque geometry carries none either.

## What would falsify it

a capture containing a draw with colour mask 7 AND blending on with a SRC_ALPHA destination factor -- tools/pass_structure.py surfaces the colour mask per draw, so re-run it on any new capture before trusting this
