---
id: 167
title: Concrete shader identity needs live Xenia command evidence
status: open
created: 2026-08-30
updated: 2026-09-04
state_items: S003,S012
tags: native-rhi,shader,xenia,re
---

## Root cause

Gears 1 package shader containers are templates. Vertex fetch state is patched
before the GPU consumes microcode, so a setter object cannot establish concrete
shader identity. The authoritative identity is the bounded `IM_LOAD` or
`IM_LOAD_IMMEDIATE` payload executed by the Xenos command stream.

## Retained evidence

Original guest function `0x822346A8` emits ordered shader loads and may roll its
command storage through `0x82221980`. The game-authored command-list interpreter
at `0x8223B2AC` transports UE3/Xenos command records; that term describes title
logic, not a CPU execution mode.

## Required resolution

Observe selected shader payloads through the authenticated Xenia device callback,
join them to the corresponding logical draw, and retain explicit missing,
malformed, predicated, rollover, and mismatch outcomes. Do not infer identity from
package templates or preserve a prior module after missing evidence.
