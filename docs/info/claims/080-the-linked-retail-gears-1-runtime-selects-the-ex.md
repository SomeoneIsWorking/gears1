---
id: C080
kind: claim
status: holds
created: 2026-08-24
tags: title-profile,provisioning,runtime,recompiler
depends: runtime/main.cpp, runtime/title_executable.cpp#LoadTitleExecutable, runtime/generated_title_profile.cpp#GeneratedTitleProfiles, runtime/title_profile.cpp#ResolveTitleProfile, extern/XenonRecomp/XenonRecomp/recompiler.cpp#Recompile
reconfirmed: 2026-08-24
verified_at: 2026-08-24 22:41:02
---

## Claim

The linked retail Gears 1 runtime selects the exact XEX container and normalized image before save state or guest activation; the generated module and runtime agree on both SHA-256 digests, base, size, and entry point.

## Evidence

Fresh retail XenonRecomp output ppc_config.h contained container df1041da...efe2d1, normalized image f61cc78e...ed5c, base 0x82000000, size 0xCE0000, entry 0x82612BF0; all 191 generated translation units compiled; generated-title synthetic mismatch tests passed; a 30-second ./run.sh --headless smoke logged selected title before save namespace and guest memory, then rendered through gameplay frames.

## What would falsify it

if any executable whose five-field XexIdentity differs from the linked generated profile reaches save loading, GuestMemory reservation, function-map installation, or _xstart

## Re-confirmed 2026-08-24

At d739721 with XenonRecomp c02a522: fresh retail generation sealed both executable digests and exact geometry, all 191 generated translation units compiled, all 64 parent CTests and all 9 sanitizer-backed XenonRecomp CTests passed, and the headless smoke selected the exact title before save namespace and GuestMemory then entered guest execution.
