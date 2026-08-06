---
id: I022
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

GEARS_PROBE_AFTER_RESOLVE=1 (Xenia fork): probe each resolve's destination in shared memory the moment its own submission completes

## Validated by

Run against BOTH classes. Positive: on our bright_delta.xtr it reports 58-84% non-zero for six destinations that the swap-time probe reported as 0.0%, so it can see a resolve landing. Negative: it still reports 0.0% for 0CB91000 and for the final composite 00311000 in the same run, so it is not manufacturing hits. The EndSubmission(true) flush before the readback is load-bearing -- without it the copy is still in the deferred command buffer and every destination would read empty, which is exactly the false negative this instrument exists to correct.

## Known failure modes

(none recorded yet)
