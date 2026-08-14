---
id: C065
kind: claim
status: holds
created: 2026-08-14
tags: render,oracle,first-divergence
depends: runtime/gpu_draw.cpp#RenderFrameImpl
---

## Claim

On the exact chapter-45 captured state, the first semantically complete colour handoff, srcC400 1280x720 f32 after the two predicated bands, already differs between native and the Gears 1 Xenia oracle.

## Evidence

tools/resolve_exact.py over scratch/ch45_exact_replay/native_exact_f0 resolve_00 and oracle_raw_f0 copies 0+2: 2,744,707 of 2,764,800 RGB half-float components differ; 921,344 of 921,600 pixels; first divergence pixel (0,0) R, native half bits 8810 versus oracle 0.

## What would falsify it

an exact-state replay of the same capture, with raw RGBA16F native output and correctly untiled contiguous oracle bands, reports the composed C400 pass bit-identical
