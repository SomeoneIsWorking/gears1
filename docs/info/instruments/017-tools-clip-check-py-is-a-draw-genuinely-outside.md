---
id: I017
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

tools/clip_check.py (is a draw genuinely outside the frustum?)

## Validated by

--selftest runs both classes on the RIGID path: a known-rasterised draw (courtyard 288) must land inside, two known-killed draws (286, 287) outside, and a deliberately wrong view-projection must FAIL the known-visible case. Re-verified live 2026-08-06 on courtyard.gfr: calibration OK on draw 288, 286/287 outside. CAUGHT LYING 2026-08-06 and fixed the same day: pointed at a SKINNED draw it applied the rigid layout (c0..c3 world, c7..c10 view-projection) to what are actually bone-palette rows and reported every vertex of character_auto.gfr draw 520 as BEHIND THE CAMERA -- a draw the GPU had rasterised into 4306 primitives. It now REFUSES a skinned draw outright (the runtime states the shader's constant-addressing mode in the VS_CONSTS header, so the data carries its own applicability), prints the calibration verdict BEFORE the per-draw numbers instead of after, and exits non-zero when nothing was computed. The self-test gained the skinned case, a static case, and a pre-field log that must read as UNKNOWN rather than as static. BLIND SPOT: it still cannot say where a skinned mesh lands -- that needs the skinning path implemented, and until then catalog #77 has no evidence any character draw is wrongly clipped.

## Known failure modes

(none recorded yet)
