---
id: C094
kind: claim
status: holds
created: 2026-08-28
tags: native-rhi,render-target,gamma,gears1
depends: runtime/titles/gears1/color_write_gamma_state.h#ApplyNativeColorWriteGammaState, runtime/titles/gears1/color_write_gamma_override.cpp#sub_82229B28, runtime/titles/gears1/rhi_target_descriptor_watch.cpp#MaybeArmRhiTargetDescriptorWriteWatch
reconfirmed: 2026-08-30
verified_at: 2026-08-30 05:10:13
---

## Claim

Gears 1 function `0x82229B28` owns the color-write gamma/sRGB state transition that mutates the bound slot-zero target descriptor after binding.

## Evidence

The binder-paused guest write watch attributed the previously unexplained `0x302D0`→`0xC02D0` write to retained guest `0x82229B28` at host RIP `0xE837CE`. Direct body inspection shows an unconditional requested-state store at device `+0x2DEC`, a slot-zero object read at `+0x2F88`, surface-format pair mappings 2↔10 and 3↔12 in object `+0x1C` and device `+0x2804`, and dirty bit 37 at device `+0x18`. Focused tests exercise all four transitions, matching/no-target/unsupported cases, and retained arithmetic for a non-boolean input. A live transactional audit matched 256/256 native calls byte-for-byte and preserved the required context, including two actual format transitions; the same run matched 3,798 draws, 15,724 bindings, and 600 presents with zero semantic errors.

## What would falsify it

Falsified if an audited input produces a different retained write set, if another guest entry mutates the same format nibble outside a target bind, or if a supported format transition does not follow the recovered pair mapping.

## Re-confirmed 2026-08-30

Committed c19182d retains and super-calls 0x82229B28 while observing its post-call slot-zero descriptor state; the frame-660 headless run matched all 15,306 color-write transitions with zero missing active targets or mismatches, and focused transition controls remain green.
