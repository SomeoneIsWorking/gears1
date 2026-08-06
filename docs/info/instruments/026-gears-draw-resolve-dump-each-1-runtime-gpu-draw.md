---
id: I026
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

GEARS_DRAW_RESOLVE_DUMP_EACH=1 (runtime/gpu_draw.cpp): snapshot each resolve destination immediately after that resolve executes

## Validated by

Positive and negative both observed on bright.gfr: it distinguishes 0C7F9000 at draw 740 (28.9%) from the SAME address at draw 756 (83.9%), which the per-target dump reports as one number -- so it can see a destination change between resolves. It reports 0.0% for 0CB91000, so it is not manufacturing coverage. KNOWN BLIND SPOT, stated by the tool: it hooks ResolveSurfaceTo only, so DEPTH resolves are absent entirely (our renderer serves those by a k_24_8_FLOAT decode path) and their absence must not be read as a missing resolve. The summary line always prints captured/executed/contained counts and warns when the frame executed no resolves at all.

## Known failure modes

(none recorded yet)
