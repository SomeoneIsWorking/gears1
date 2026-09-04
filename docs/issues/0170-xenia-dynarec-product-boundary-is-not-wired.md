---
id: 170
title: Xenia dynarec product boundary is not wired
status: open
symptom: GearsUE3 still contains only the XenonRecomp-generated gameplay executor, while the target product must execute the user XEX through Xenia's x64/A64 dynarecs
tags: dynarec,xenia,xenonport,migration
state_items: S009,S010,S011,S012
created: 2026-09-04
updated: 2026-09-04
---

## Root cause

The previous architecture made a generated C++ corpus the guest execution
owner. The new product contract requires runtime translation from the
authenticated user executable, but no shared `xenonport` embedding boundary or
Gears adapter exists yet.

## Required work

Create `xenonport` around Xenia `Memory`, `Processor`, `ThreadState`,
`RawModule`, typed imports, device-memory callbacks, runtime overrides, scoped
original calls, bounded exits, and Xenia-owned invalidation. Reuse Xenia's x64
and A64 dynarecs; do not implement a PPC interpreter or put Xenia behind
`jit-common` code-memory/cache owners.

The first Gears discriminator executes real leaf `0x8222E868`, a typed
`DbgPrint` import, and disabled/enabled/`super` override paths through Xenia.
Then expand to representative interactive gameplay and the full migration gate
in `shared/jit-common/docs/migration.md`.

## Retirement condition

Do not regenerate, build, or run the XenonRecomp product. Preserve its current
tree as prior evidence until representative gameplay passes. Then delete the
generator, generated-module build path, precomputed function maps,
generator-only configuration/tests, and static methodology in the same
milestone; no compatibility selector remains.
