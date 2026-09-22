---
id: 153
title: Native audio mix needs Xenia dispatch
status: resolved
created: 2026-08-28
updated: 2026-09-22
state_items: S004,S006
tags: performance,audio,native-engine,xenia
---

## Retained contract

`runtime/titles/gears1/audio_mix.*` owns an independently authored SIMD kernel for
original guest function `0x825F7B40`. The recorded address was previously
`0x825F2D40`, a normalized-image offset mistaken for a virtual address.

## Resolution

`Gears1Runtime::ComposeBindings` installs the override only when the authenticated
image contains the exact address, and `tests/test_gears1_real_leaf.cpp` compares
the native override against `CallOriginal` on the real image: the return value and
all 320 output words agree exactly. Fixing the cause required the corrected
address, reproducing the guest's r3 result (the last processed input block, not
the output pointer), and a fused multiply-add with denormal-input flushing to
match `vmaddfp`.

Performance measurement is not part of this issue and still requires dynarec
execution; interpreter fallback samples do not qualify.
