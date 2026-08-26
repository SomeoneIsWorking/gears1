---
id: I005
kind: instrument
status: trusted
created: 2026-08-27
---

## Instrument

GEARS_NATIVE_RHI_OBSERVE semantic draw/binding comparator

## Validated by

Focused tests feed matching and deliberately altered draw packets and binding
state, observe both answers, and prove one global sequence across event kinds. A
headless menu walk through frame 1712 produced 90,854 draw matches with zero
missing or mismatched packets; a later headless run through frame 120 produced
970 texture/shader binding matches with zero missing or mismatched state.

## Known failure modes

(none recorded yet)
