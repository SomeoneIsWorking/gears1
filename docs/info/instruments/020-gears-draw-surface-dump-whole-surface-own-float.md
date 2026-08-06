---
id: I020
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

GEARS_DRAW_SURFACE_DUMP (whole surface, own float format, after a named DIAG draw)

## Validated by

Self-tested three ways on bright.gfr: (1) POSITIVE -- diag 460 dumps 126,983 sentinel-marked fragments and names the matching ps hash f662d670789bfac0; (2) NEGATIVE -- diag 99999 reports 'the frame's draws ran to diag index 842, so this is never offered, NOT wrote nothing'; (3) the debug shader ported instruction-for-instruction reproduces the REAL shader's output through this probe to <=1e-3 on 99.76% of the draw's own fragments, both reporting exactly 99.8% zero. Also correctly reports NO COVERAGE on character_auto.gfr, whose draw 319 is killed at clip (claim C017).

## Known failure modes

(none recorded yet)
