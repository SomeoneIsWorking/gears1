---
id: 153
title: Native audio mix kernel removes a hot recompiled loop
status: resolved
symptom: A current gameplay profile attributes a large share of process CPU to retained function 0x825F2D40, the title's 16-iteration SIMD audio mix kernel
state_items: S004,S007
tags: performance,audio,native-engine,recomp,gears1
created: 2026-08-28
updated: 2026-08-28
---

## Root cause

The recompiler faithfully executes the audio mixer as thousands of translated vector loads,
stores, and arithmetic operations. That is the real cost center: the mixer has no external calls or
blocking behavior, so the host can execute its established vector operation directly while keeping
the generated body available as the retained authority.

## Fix

`runtime/titles/gears1/audio_mix.cpp` is an independently written SIMD translation of the grounded
Gears 1 body. `audio_mix_override.cpp` binds it only when `GEARS_NATIVE_AUDIO_MIX=1`, retains
`__imp__sub_825F2D40` for `GEARS_RECOMP_AUDIO_MIX=1`, and provides an in-process alternating audit.
The audit compares 1024 output bytes after each native/recompiled same-call pair and restores the
entry context before invoking the retained body. The native implementation also preserves the
retained stack stores and guest-vector byte order.

## Evidence

On the current Clang build, the retained profile `scratch/bin/perf-current-gameplay.data` put
`__imp__sub_825F2D40` at 18.59% of sampled cycles. A same-path native profile,
`scratch/bin/perf-native-audio.data`, contains no samples for that symbol. In the native run,
warm-up gameplay rendered at 29.6-30.0 frames/s, compared with the earlier retained heavy phase
at about 15-16 frames/s. A subsequent headless run with
`GEARS_NATIVE_AUDIO_MIX_AUDIT=1` reported `native audio mix audit matched 256 call(s)` and no
divergence or abort.

## Falsifier

This is falsified by any same-call audit mismatch, by a current profile showing the native path
still spending material CPU in `__imp__sub_825F2D40`, or by a controlled native run producing
different audio-frame bytes or guest behavior. It does not establish the complete native renderer
or the separate title-side 30 Hz production limit.
