---
id: C023
kind: claim
status: holds
created: 2026-08-07
tags: oracle,render,shaders,gameplay-scene
depends: runtime/gpu_draw.cpp
---

## Claim

Every VERTEX shader the console binds, our runtime also binds; but EIGHT pixel shaders are bound on the console and never on ours, and none the other way.

## Evidence

Live draw-stream comparison, both cores driven to Act 1 independently with no trace replay (ours 7,461 frames/3.66M draws; oracle 6,005/6.49M). VS: ours 62, theirs 61, set difference EMPTY in the oracle->us direction. PS: ours 262, theirs 270; the eight are 2cff262892b471cc 576a520e27020b3e 5c89d93b82909724 716db212afe61ac9 84e14f58de37e54b b72e0f5009ac6f2a dbd9703d3a7104bc ebbfc05467ebde02, and zero go the other way. Three of the VSs they pair with first appear exactly when gameplay starts (frames 1248-1257) and persist. Our frame of the same scene has no character and no HUD. tools/draw_stream_compare.py on scratch/oracle/stream/{ours,theirs}.tsv.

## What would falsify it

if our runtime DOES bind those pixel shaders but the draw-stream emitter never sees them -- it records from , so a draw dropped before preparation is invisible to it. Check by hashing every PS the guest programs at the register level, not at the prepared-draw level, before concluding the guest never asks for them.
