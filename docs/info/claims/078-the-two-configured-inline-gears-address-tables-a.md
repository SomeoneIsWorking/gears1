---
id: C078
kind: claim
status: holds
created: 2026-08-24
tags: recompiler,switch
depends: config/gears.toml, extern/XenonRecomp/XenonAnalyse/function.cpp, extern/XenonRecomp/XenonRecomp/function_scan.cpp, extern/XenonRecomp/XenonRecomp/switch_extent.cpp
reconfirmed: 2026-08-24
verified_at: 2026-08-24 22:41:03
---

## Claim

The two configured inline Gears address tables are data holes inside disjoint switch owners, and every one of their eight case targets emits as a local block rather than a standalone function.

## Evidence

Fresh XenonRecomp generation reached 100% exit 0; each target had exactly one goto and one local label, table-word addresses had zero functions, and generated output had zero ERROR/unreachable/unrecognized matches.

## What would falsify it

Fresh generation for the exact configured image emits a target as sub_*, decodes a table word as a function, changes label/goto cardinality, or reports any switch ownership error.

## Re-confirmed 2026-08-24

Re-verified against XenonRecomp commit a841864: exact Gears generation reached 100% with eight one-label/one-edge local targets, zero standalone case functions, zero table-word functions, and zero generated error markers.

## Re-confirmed 2026-08-24

Confirmed after parent commit ae7df48: the committed exact data ranges are the inputs used by the clean 100% generation audit with local-only case blocks and no decoded table words.

## Re-confirmed 2026-08-24

At d739721 with XenonRecomp c02a522: switch ownership tests passed under ASan/UBSan, the overlapping-block regression proved normalized emission produces one label, and clean retail generation reached 100% with all 191 generated translation units compiling.
