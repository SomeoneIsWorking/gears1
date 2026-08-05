---
id: I007
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/repro_rate.sh

## Validated by

Measures how often a rare failure reproduces across N concurrent runs. WAS BLIND to hang-shaped bugs until 2026-08-05: it classified by exit code alone, so #44's stalled-but-alive guest returned 124 from timeout and was counted CLEAN -- the project's top blocker scored as a pass. Now has three outcomes (crashed/stalled/clean), reads runtime/wait_probe.cpp's own stall verdict rather than a frame threshold, reports how many runs ARMED the detector so '0 stalled' carries its denominator, and ships --selftest which is verified to FAIL when the classifier pattern is broken. Do not trust a rate from a build where --selftest does not pass.

## Known failure modes

(none recorded yet)
