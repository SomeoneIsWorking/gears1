---
id: C100
kind: claim
status: holds
created: 2026-08-28
tags: native-engine,native-rhi,resource,re,gears1
depends: runtime/titles/gears1/rhi_resource_construction_binding.cpp, runtime/rhi_semantic_stream.h
---

## Claim

Gears 1 resource-wrapper constructors `0x8222EA18` and `0x8222EB78` have distinct, statically
grounded contracts and their retained post-call wrapper words are represented by the semantic
stream without enabling a native construction path

## Evidence

The generated bodies allocate 32-byte wrappers, set refcount one and sentinel `0xFFFF0000` at
`+20`, and initialize backing/size state at `+24`/`+28`. `0x8222EA18` consumes requested bytes in
`r3` and resource flags in `r4` for owned backing; `0x8222EB78` additionally consumes allocation
flags in `r5` for wrapped backing. Their known caller at `0x827DA348` passes `(12, 0, 1, 0)` and
`(120, 0, 0, 0)`. The current headless semantic walk reached frame 1440 with zero construction
events, so the contracts are static-only and the retained bodies remain authoritative.

## What would falsify it

A generated-body or dynamic call-site observation showing different argument consumption or
post-call field meaning, or a headless run that reports a construction event whose retained object
does not satisfy the recorded wrapper layout.
