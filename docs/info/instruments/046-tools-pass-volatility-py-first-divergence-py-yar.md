---
id: I046
kind: instrument
status: DISTRUSTED
created: 2026-08-12
distrusted_on: 2026-08-13
---

## Instrument

tools/pass_volatility.py + first_divergence.py --yardstick: the console against ITSELF one guest frame apart, per pass, as the denominator for any cross-emulator pass score

## Validated by

Run on scratch/camerapair_ps/theirs, 40 consecutive frames 793..832. Reports a spread from 0.0962 (a C2D0 f7 mask ordinal) to 0.9742 (the shadow atlas), which is the point: judging both against one threshold prices two different volatilities the same. Both classes driven in --selftest -- a pass with only sensor noise must read >0.90 and one whose content is genuinely redrawn must read <0.50; a yardstick that called both stable would certify everything. Keys that appear in only some frames are reported with their presence count (5/40, 22/40) rather than silently averaged over the frames they happened to appear in.

Re-validated 2026-08-13 on `scratch/camerapair/theirs`, 60 consecutive frames 793..852. The f6 mask family spans 0.1272..0.7909 over four adjacent pairs; its front buffer is 0.4727. Constant pair(s) are reported as such, not fed to correlation as `nan`. The tool's `--selftest` still drives both stable and genuinely-redrawn synthetic classes after this change.

## Known failure modes

Before 2026-08-13, the tool invoked the full flip/shift same-picture search for every temporal pair and then discarded it, retaining only the unshifted correlation. At 720p that created hundreds of full-frame temporaries per pair and the process died partway through the report; partial output could be mistaken for a completed yardstick. The temporal path now asks explicitly for unshifted correlation, which is also semantically correct: spatial searching would hide the motion the tool is meant to price. A constant source or destination has undefined correlation; it is counted and printed as `CONSTANT`, never ranked as a numeric volatility score.

## DISTRUSTED 2026-08-13

It priced every pass at exactly +1 console frame and then still detected a loss from the raw correlation drop versus the previous, unrelated pass. On the equal-state pair priced by pair_score at 1.13 frames, this named f7 because raw r fell 0.2125 from velocity; f7 is actually only 0.0117 below its own interpolated self curve. Cross-pass raw correlations are not comparable.

> Every result this instrument produced is suspect until it is re-validated.
