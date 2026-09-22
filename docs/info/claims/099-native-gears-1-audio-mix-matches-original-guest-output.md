---
id: C099
kind: claim
status: holds
created: 2026-08-28
tags: performance,audio,native-engine
depends: runtime/titles/gears1/audio_mix.cpp
---

## Claim

The native Gears 1 audio-mix kernel reproduces the output of original guest
function `0x825F7B40` bit-exactly when the original is executed through Xenia.

## Evidence

`tests/test_gears1_real_leaf.cpp` allocates a guest fixture, runs the original
function through `CallOriginal` on the authenticated real image, re-seeds the
fixture, dispatches the native override through `Execute`, and compares the
return value and all 320 output words. They agree exactly.

Two divergences were found and fixed at their cause while establishing this:
the guest returns the last processed input block in r3 rather than the output
pointer, and `vmaddfp` is a fused multiply-add that flushes denormal inputs to
signed zero, so the kernel uses `std::fma` rather than a separate multiply and
add. The previously recorded address `0x825F2D40` was a normalized-image offset
mistaken for a virtual address, displaced by the 0x4E00 `.text` alignment gap;
executing it ran an unrelated epilogue thunk.

## What would falsify it

Any same-input divergence in the return value or output words between the native
override and the original guest function executed through Xenia, on the exact
authenticated revision. A host whose Xenia build lacks FMA3 lowers `vmaddfp` to
a separate multiply and add, so a divergence there falsifies the reference, not
the kernel.
