---
id: 126
title: Gears 1 stays near 30 fps despite a 60 Hz host vblank and disabled rendering
status: investigating
symptom: headless guest presents at 29.7-29.9 fps and must reach verified 60 fps without speeding the guest clock
tags: performance,timing,60fps,present,vblank
created: 2026-08-22
updated: 2026-08-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-08-22)
Bounded headless runs establish a 60 Hz host vblank and roughly 29.9 guest
VdSwaps/s. The live D3D presentation-sync setting is 2 and setup maps 1/2/4 to
distinct vblank modes, but forcing mode 1 before initial notification
registration still measured about 30.2/30.9 VdSwaps/s; interval two is not
causal. The decompiled 1/60 accumulator is a render-thread viewport/platform
tick that polls controllers and calls Present, not proof of a 60 Hz simulation
scheduler. The sole exact 1/30 constant belongs to an outer engine fixed-step
branch whose live enable flag is zero, so that branch is inactive. Cadence
ownership remains behind the indirect UE3 game-thread/render-command producer
chain upstream of the render-thread Present tick. Instrument that semantic tick
and render-command enqueue cadence/delta next; no Present, vblank, clock, or
constant override is justified by current evidence.

Priority: defer this per-game enhancement until Gears 1 is stable and performs
well enough at its faithful cadence. Renderer/native-engine performance and
glitch prevention remain active work; 60 fps is the final override milestone.
