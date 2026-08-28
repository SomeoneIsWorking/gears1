---
id: I064
kind: instrument
status: trusted
created: 2026-08-28
---

## Instrument

`GEARS_NATIVE_RHI_PLAN=1` PM4-independent semantic frame-plan boundary

## Validated by

`tests/test_native_rhi.cpp` accepts a complete ordered frame and rejects missing
evidence, non-increasing event sequences, missing presents, non-terminal
presents, and missing construction evidence. A Clang headless Gears 1 menu walk
through frame 1440 accepted every live semantic frame and logged the resulting
plan counts.

## Known failure modes

The plan is not a renderer. It does not execute a host backend, prove state or
pixel parity, or establish a performance result. The compatibility PM4 path
continues to produce the frame. Native execution must not be enabled based on
this instrument alone. In a longer live walk, later frames were refused when
the pre-existing resolve observer reported missing or mismatched retained
packet evidence; that refusal is expected and identifies an unresolved parity
gap rather than a plan failure.
