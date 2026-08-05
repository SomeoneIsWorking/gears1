---
id: I006
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/frame_hashes.sh

## Validated by

Renders all 8 captures and hashes each frame; catches any change to the PICTURE. BLIND SPOT, established 2026-08-05: it sets no GEARS_DRAW_* knob, so FRAME_STEP, PIXEL_TRACE, DIAG, the dumps and the A/B arms are dead code in every run it makes -- a clean report says nothing about a refactor of those. Refactors of the probes this session were gated by RUNNING each knob and diffing its output (checkpoint PPM, pixel trace, 163-row DIAG table) against the previous build instead.

## Known failure modes

(none recorded yet)
