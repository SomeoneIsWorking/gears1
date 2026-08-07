---
id: C023
kind: claim
status: falsified
created: 2026-08-07
tags: oracle,render,shaders,gameplay-scene
depends: runtime/gpu_draw.cpp
falsified_on: 2026-08-07
---

## Claim

Every VERTEX shader the console binds, our runtime also binds; but EIGHT pixel shaders are bound on the console and never on ours, and none the other way.

## Evidence

Live draw-stream comparison, both cores driven to Act 1 independently with no trace replay (ours 7,461 frames/3.66M draws; oracle 6,005/6.49M). VS: ours 62, theirs 61, set difference EMPTY in the oracle->us direction. PS: ours 262, theirs 270; the eight are 2cff262892b471cc 576a520e27020b3e 5c89d93b82909724 716db212afe61ac9 84e14f58de37e54b b72e0f5009ac6f2a dbd9703d3a7104bc ebbfc05467ebde02, and zero go the other way. Three of the VSs they pair with first appear exactly when gameplay starts (frames 1248-1257) and persist. Our frame of the same scene has no character and no HUD. tools/draw_stream_compare.py on scratch/oracle/stream/{ours,theirs}.tsv.

## What would falsify it

if our runtime DOES bind those pixel shaders but the draw-stream emitter never sees them -- it records from , so a draw dropped before preparation is invisible to it. Check by hashing every PS the guest programs at the register level, not at the prepared-draw level, before concluding the guest never asks for them.

## FALSIFIED 2026-08-07

The stated falsifier is RESOLVED IN OUR FAVOUR: GEARS_DRAW_STREAM_RAW records
the (vs,ps) pair at the TOP of the draw loop, above all ten drop sites, and over
a 12,257-frame run it reports 0 pairs programmed and never prepared. Our
renderer drops nothing.

The claim is falsified by a DIFFERENT defect it did not consider: THE TWO SIDES
WERE NEVER DRIVEN ALONG THE SAME WALK. Ours ran a millisecond menu walk that
stops pressing at 120 s. The oracle ran "START@150+270,A@300+120", which goes on
pressing START -- which is PAUSE once the level is up -- and A every 120 frames
for the entire run. Neither side had any movement input, so neither walked
anywhere on purpose. The two runs were at different points in the game for every
gameplay frame, so a set difference over whole runs measures HOW FAR EACH SIDE
GOT, not what each side renders.

Concretely: the oracle's frame 6000 shows Marcus holding a Lancer with 312
rounds, while our run is still in the unarmed prologue. The missing HUD, read as
evidence for this claim, is a weapon the player does not have yet.

Superseded by the shared frame-keyed walk: ONE table in tools/menu_walk.sh with
gears_walk_ours/gears_walk_theirs generating both notations, used by
tools/oracle_lockstep.sh, which now cross-checks that both sides act at the same
guest frames and proves that check fires before trusting it.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
