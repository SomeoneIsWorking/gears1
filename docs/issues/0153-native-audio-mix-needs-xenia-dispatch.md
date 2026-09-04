---
id: 153
title: Native audio mix needs Xenia dispatch
status: open
created: 2026-08-28
updated: 2026-09-04
state_items: S004,S006
tags: performance,audio,native-engine,xenia
---

## Retained contract

`runtime/titles/gears1/audio_mix.*` owns an independently authored SIMD kernel for
original guest function `0x825F2D40`. Historical same-call evidence is recorded in
claim C099, but the old comparison wrapper and selectors were deleted.

## Required resolution

Bind the function only for the exact authenticated Gears 1 revision, compare it
against the original guest function through Xenia, and retain the native route
only if state and output agree. Performance measurements require dynarec execution;
interpreter fallback samples do not qualify.
