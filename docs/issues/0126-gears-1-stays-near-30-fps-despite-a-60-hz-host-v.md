---
id: 126
title: Gears 1 stays near 30 fps despite a 60 Hz host vblank and disabled rendering
status: investigating
symptom: headless guest presents at 29.7-29.9 fps and must reach verified 60 fps without speeding the guest clock
tags: performance,timing,60fps,present,vblank
created: 2026-08-22
updated: 2026-08-29
---

## Root cause


## What was tried / dead ends

### Title-boundary timing trace (2026-08-28)

`GEARS_FRAME_PRODUCTION_TRACE=1` now counts the exact Gears 1 boundaries while
retaining and super-calling the generated bodies: `0x8221B378` (timing tick),
`0x8221B670` (Bink/render producer dispatch), `0x824A5170` (producer present
wrapper), and the host render handoff; the existing `VdSwap` rate log is the
cross-check. A 12-second headless boot trace
measured the timing tick at roughly 4.7–5.5 million calls/s, producer dispatch
at 70–113/s, and producer-present calls equal to `VdSwap` at 22–29/s. The
remaining dispatches took the `0x82AE8C30` blocked path.

This does not identify the gameplay limiter: issue #0013 already identifies
`0x82AE8C30` as the Bink wait and `0x8221B670` as the Bink pump, so these counts
describe the startup movie path. The probe rules out treating that movie wait
as the title's general 30 Hz simulation cap. The next measurement must reach a
gameplay-state producer after the Bink path; no timing or present override is
justified by this trace.

### Post-Bink ring and VdSwap trace (2026-08-29)

The trace was extended to the render-ring reservation entry and the semantic
present boundary (which is emitted once per `VdSwap`), then
run headlessly for 45 seconds with host rendering bypassed
(`GEARS_DRAW_FRAME_AT=99999999`, `GEARS_DRAW_FRAME_COUNT=0`). At approximately
frame 571, the startup producer counters stopped at 993 dispatches and 429
blocked calls, while the ring recorded 7,971 reservations and `VdSwap` recorded
571 calls at 29.9/s. By approximately frame 1,142, the producer counters were
still unchanged, ring reservations had reached 81,643, and `VdSwap` remained
29.9/s. Render handoff stayed at zero as requested by the no-render control.

This proves the earlier trace stopped at an instrumentation boundary: ring
activity continues after the Bink producer boundary, but it does not identify
which gameplay producer or wait limits `VdSwap`. It is not a timing fix, and
the no-render control still parses guest PM4 and accumulates draw commands, so
it cannot establish native-engine cadence. The next instrument must classify
the post-Bink ring producer/consumer interval or the guest wait that precedes
it; a 60 Hz override remains unjustified.


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
