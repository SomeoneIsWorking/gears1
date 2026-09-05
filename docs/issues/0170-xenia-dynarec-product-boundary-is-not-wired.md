---
id: 170
title: Xenia dynarec product boundary is not wired
status: open
symptom: GearsUE3 exercises x360port/Xenia synthetically, but the authenticated full-image product and runtime services are not composed
tags: dynarec,xenia,x360port,x360ue3,migration
state_items: S002,S006,S007,S008,S009,S010,S011
created: 2026-09-04
updated: 2026-09-04
---

## Root cause

The previous architecture made a generated C++ corpus the guest execution
owner. The new product contract requires runtime translation from the
authenticated user executable. The shared `x360port` embedding boundary and a
Gears-owned synthetic discriminator now exist, but the exact-title full-image
adapter and the device, override, invalidation, exit, and fallback contracts do
not yet compose a product.

## Required work

Extend the current `x360port` ownership of Xenia `Memory`, `Processor`,
`ThreadState`, `RawModule`, typed imports, and guest calls with device-memory
callbacks, runtime overrides, scoped original calls, bounded exits, and
Xenia-owned invalidation. Reuse Xenia's x64 and A64 dynarecs; do not write a
second PPC interpreter or put Xenia behind `jit-common` code-memory/cache owners.
Expose Xenia's existing interpreter only as the bounded, reason-labelled
fallback and explicit diagnostic mode defined by the project goals.

The asset-free Gears discriminator now translates an aligned synthetic image
whose code and entry point use retained leaf address `0x8222E868`, calls a typed
`DbgPrint` import, and returns to native code. The next discriminator executes the authenticated real leaf and
disabled/enabled/scoped-original override paths through Xenia. Then expand to
representative interactive gameplay and the full migration gate.

The executor must select Xenia's A64 dynarec for both Apple Silicon macOS and
Android `arm64-v8a`, with dynarec selected by default and only the bounded,
reason-labelled fallback policy described in the project goals. Qualify executable
memory, instruction-cache coherence, host ABI, and runtime packaging separately
on those two platforms rather than inferring one from the other.

## Break-first condition

Satisfied: the retired translator submodule, generated-module build path, precomputed
function maps/import profile, generation configuration/tools/tests, obsolete entry
and indirect dispatch, and stale-product selector are gone. Independent evidence,
native subsystem contracts, and the reusable checked-XEX parser contract remain.
The launcher and named product target now refuse at missing full-image and
runtime-service composition over `x360port`; there is no compatibility CPU
selector.
