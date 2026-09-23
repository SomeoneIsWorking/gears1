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
function `0x825F2D40` bit-exactly when the original is executed through Xenia.

## Evidence

`tests/test_gears1_real_leaf.cpp` allocates a guest fixture, runs the original
function through `CallOriginal` on the authenticated real image, re-seeds the
fixture, dispatches the native override through `Execute`, and compares the
return value and every byte of the arena, with the input and output blocks
disjoint, in place, and overlapping by one iteration either way. They agree
exactly; forcing the overlapping blocks to be staged separately makes 239 of 768
words differ, so the comparison sees aliasing.

Two divergences were found and fixed at their cause while establishing this:
the guest returns the last processed input block in r3 rather than the output
pointer, and `vmaddfp` is a fused multiply-add that flushes denormal inputs to
signed zero, so the kernel uses `std::fma` rather than a separate multiply and
add. The kernel was first qualified on a runtime image re-laid by section
VirtualAddress, which carried this body at `0x825F7B40`; on the image the XEX
loader actually produces (and the product runs) it is at `0x825F2D40`, and the
comparison was re-run and agrees there (2026-09-23).

## What would falsify it

Any same-input divergence in the return value or output words between the native
override and the original guest function executed through Xenia, on the exact
authenticated revision. A host whose Xenia build lacks FMA3 lowers `vmaddfp` to
a separate multiply and add, so a divergence there falsifies the reference, not
the kernel.
