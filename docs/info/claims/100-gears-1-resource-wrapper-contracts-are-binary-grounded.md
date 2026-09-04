---
id: C100
kind: claim
status: holds
created: 2026-08-28
tags: native-engine,native-rhi,resource,re,gears1
depends: docs/d3d-seam.md
---

## Claim

Original guest functions `0x8222EA18` and `0x8222EB78` construct distinct
32-byte Gears 1 resource wrappers with reference count one, sentinel
`0xFFFF0000` at `+20`, and backing/size fields at `+24/+28`.

## Evidence

Executable analysis shows `0x8222EA18` consumes requested bytes in `r3` and
resource flags in `r4`; `0x8222EB78` additionally consumes allocation flags in
`r5`. The known caller at `0x827DA348` passes `(12, 0, 1, 0)` and
`(120, 0, 0, 0)`. No live construction call was recorded.

## What would falsify it

A call observed through the authenticated Xenia context with different argument
consumption or post-call wrapper fields.
