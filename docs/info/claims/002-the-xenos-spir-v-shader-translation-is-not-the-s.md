---
id: C002
kind: claim
status: holds
created: 2026-08-05
tags: gpu,shaders,native-renderer
depends: runtime/shaders/movie_yuv.frag, runtime/shaders/scene_gamma.frag
---

## Claim

The Xenos->SPIR-V shader translation is NOT the source of the graphics defect

## Evidence

Two pixel shaders reimplemented by hand from the title's microcode -- the movie YUV composite (0xea0007942db096ad) and the full-screen gamma/exposure composite (0x501ac5d8692bf7b6) -- each render 2764800 of 2764800 channel samples IDENTICAL to the translated module, on two different captures (boot150.gfr, act1.gfr), via tools/verify_native_pass.sh with its negative control passing in the same run

## What would falsify it

a native pass that disagrees with the translation. This covers TWO full-screen composites with 1 and 3 texture fetches and no flow control beyond one uniform branch; it says nothing about vertex shaders, about the base pass, or about material shaders with loops, gradients or signed/biased fetches
