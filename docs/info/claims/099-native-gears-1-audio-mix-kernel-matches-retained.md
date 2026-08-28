---
id: C099
kind: claim
status: holds
created: 2026-08-28
tags: performance,audio,native-engine
depends: runtime/titles/gears1/audio_mix.cpp, runtime/titles/gears1/audio_mix_override.cpp
---

## Claim

The native Gears 1 audio mix kernel matches the retained `0x825F2D40` output and removes its
recompiled CPU hotspot when explicitly enabled

## Evidence

The same-call headless audit matched 256 native executions against the retained body with no
divergence. In current Clang perf data, the retained body accounts for 18.59% of sampled cycles;
it is absent from the native profile, and the native gameplay run reaches about 30 rendered
frames/s after warm-up instead of the retained run's 15-16 fps heavy phase.

## What would falsify it

Any audit mismatch, a native profile that still attributes material CPU to the retained mixer, or a
controlled audio/gameplay output difference between the two arms.
