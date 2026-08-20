---
id: I054
kind: instrument
status: DISTRUSTED
created: 2026-08-14
distrusted_on: 2026-08-20
---

## Instrument

tools/gfr_to_xtr.py --checkpoint-copy plus tools/gfr_trace_plan.py: exact intermediate Xenia EDRAM copy selection

## Validated by

Real chapter-45 negative after draw 742 (C400 zero on both) and positive after draw 743 (oracle zero, native four pixels), plus parser/ordering self-tests

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-20

Its claimed positive was native nonzero versus oracle zero, not a positive output from the oracle checkpoint. The forced-white plus no-depth/no-stencil control generated 147,870 oracle fragments but the checkpoint resolve remained zero, proving the target checkpoint can silently lose writes.

> Every result this instrument produced is suspect until it is re-validated.
