---
id: C103
kind: claim
status: holds
created: 2026-08-30
tags: native-rhi
depends: runtime/rhi_renderer_input.cpp#CompareRhiRendererDraws, runtime/titles/gears1/rhi_bindings.cpp
reconfirmed: 2026-08-30
verified_at: 2026-08-30 03:59:06
---

## Claim

The 30 unmatched compatibility-renderer packet groups observed through frame 360 decompose into 24 fixed device-initialization point draws from 0x8222B678 and six internal rectangle clears emitted below 0x8222BC18; they do not identify a missing normal Gears 1 draw wrapper.

## Evidence

Exact-revision literal DRAW_INDX/DRAW_INDX_2 writer census plus headless GEARS_NATIVE_RHI_OBSERVE provenance run through frame 360, recorded in issue #141 Renderer-input comparison

## What would falsify it

any unmatched packet group is attributed outside 0x8222B678 or the 0x8222BC18 clear chain, or a normal content draw reaches the renderer without one of the four semantic draw wrappers

## Re-confirmed 2026-08-30

Committed tree 1eb0f94 carries the exact-revision writer census result, terminal ring/indirect provenance, ambiguity controls, and issue #141 evidence for the 24 startup plus six internal-clear decomposition.
