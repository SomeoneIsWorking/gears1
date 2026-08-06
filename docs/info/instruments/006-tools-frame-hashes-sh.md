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

**An all-black render hashed as cleanly as a picture (found and FIXED
2026-08-06).** play_v2.gfr and character_auto.gfr both render 921600 pure-black
pixels, so both hashed to 847b7f79e03d5c66 and the script reported that as an
ordinary match between two unrelated captures. Uniform output is the classic
broken-instrument tell and it was being printed as a normal row. The script now
detects a completely black frame, tags the row ALL-BLACK, and prints a count at
the end (currently 2 of 16). Its unchanged/changed answer was never wrong -- a
black frame's hash is stable and comparable -- but "everything matches" read as
health when two of the captures were rendering nothing. Both classes exercised:
14 captures are untagged, 2 are tagged.
