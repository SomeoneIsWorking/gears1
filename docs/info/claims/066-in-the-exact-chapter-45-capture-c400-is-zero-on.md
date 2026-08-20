---
id: C066
kind: claim
status: falsified
created: 2026-08-14
tags: render,oracle,first-divergence
depends: tools/gfr_to_xtr.py, tools/gfr_trace_plan.py, runtime/gpu_draw.cpp
falsified_on: 2026-08-20
---

## Claim

In the exact chapter-45 capture, C400 is zero on both renderers after draw 742 and first differs after draw 743, where native writes four pixels and Xenia writes none

## Evidence

I054 checkpoint copies at prefixes 743 and 744, native surface dumps after diagnostic draws 742 and 743, and exact zero/nonzero inspection

## What would falsify it

A replay of the same capture and prefixes produces nonzero C400 before draw 743 or matching C400 after draw 743

## FALSIFIED 2026-08-20

The Xenia checkpoint used as evidence failed a forced-white/no-depth positive control: 147,870 fragment invocations still yielded an all-zero selected resolve. Native first writes after draw 743, but oracle zero at that checkpoint is not evidence of a cross-renderer first divergence.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
