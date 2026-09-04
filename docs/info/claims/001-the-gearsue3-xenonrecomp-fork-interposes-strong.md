---
id: C001
kind: claim
status: holds
created: 2026-08-22
tags:
depends: extern/XenonRecomp
reconfirmed: 2026-08-24
verified_at: 2026-08-24 22:41:03
---

## Claim

Before the dynarec migration, the GearsUE3 XenonRecomp fork interposed strong native overrides on same-translation-unit generated calls while retaining an original-body super-call. This is historical static-path evidence, not the target dispatcher and not permission to regenerate or run the old product.

## Evidence

XenonRecomp commit 884206f CTest XenonRecompBindingTests passed; a fresh Gears generation emitted 48,892 PPC_FUNC_IMPL bodies and 48,892 PPC_WEAK_FUNC forwarders with zero aliases; the parent Clang build linked that generation

## What would falsify it

The recorded XenonRecomp revision or cited test result does not reproduce this behavior. The claim does not apply to x360port; its disabled/enabled/scoped-super paths require new evidence through Xenia.

## Re-confirmed 2026-08-24

At d739721 with XenonRecomp c02a522: the sanitizer-backed XenonRecomp CTest suite passed all binding emission/link tests after the loader extraction and fresh generated module build.
