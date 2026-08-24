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

The GearsUE3 XenonRecomp fork interposes strong native overrides on same-translation-unit generated calls while retaining an original-body super-call

## Evidence

XenonRecomp commit 884206f CTest XenonRecompBindingTests passed; a fresh Gears generation emitted 48,892 PPC_FUNC_IMPL bodies and 48,892 PPC_WEAK_FUNC forwarders with zero aliases; the parent Clang build linked that generation

## What would falsify it

if the linker regression fails, a generated same-TU call bypasses the strong override, a retained __imp__ body is missing, or generated output contains a compiler alias

## Re-confirmed 2026-08-24

At d739721 with XenonRecomp c02a522: the sanitizer-backed XenonRecomp CTest suite passed all binding emission/link tests after the loader extraction and fresh generated module build.
