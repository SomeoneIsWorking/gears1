---
id: 170
title: Xenia dynarec product boundary is not wired
status: open
symptom: GearsUE3 has removed its generated gameplay executor, but the required x360port/Xenia runtime boundary does not exist
tags: dynarec,xenia,x360port,x360ue3,migration
state_items: S009,S010,S011,S012,S013
created: 2026-09-04
updated: 2026-09-04
---

## Root cause

The previous architecture made a generated C++ corpus the guest execution
owner. The new product contract requires runtime translation from the
authenticated user executable, but no shared `x360port` embedding boundary or
Gears adapter exists yet.

## Required work

Create `x360port` around Xenia `Memory`, `Processor`, `ThreadState`,
`RawModule`, typed imports, device-memory callbacks, runtime overrides, scoped
original calls, bounded exits, and Xenia-owned invalidation. Reuse Xenia's x64
and A64 dynarecs; do not write a second PPC interpreter or put Xenia behind
`jit-common` code-memory/cache owners. Expose Xenia's existing interpreter only
as the bounded, reason-labelled fallback and explicit diagnostic mode defined by
the project goals.

The first Gears discriminator executes real leaf `0x8222E868`, a typed
`DbgPrint` import, and disabled/enabled/`super` override paths through Xenia.
Then expand to representative interactive gameplay and the full migration gate
in `shared/jit-common/docs/migration.md`.

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
The launcher and named product target now refuse at the missing `x360port`
executor boundary; there is no compatibility CPU selector.
