---
id: C103
kind: claim
status: holds
created: 2026-08-30
tags: native-rhi
depends: runtime/rhi_renderer_input.cpp#CompareRhiRendererDraws, runtime/titles/gears1/rhi_bindings.cpp#sub_8222CFF8, runtime/titles/gears1/rhi_bindings.cpp#sub_8222D4F8, runtime/titles/gears1/rhi_bindings.cpp#sub_8222DA48, runtime/titles/gears1/rhi_bindings.cpp#sub_8222DE50
reconfirmed: 2026-08-30
verified_at: 2026-08-30 05:10:14
---

## Claim

The 30 unmatched compatibility-renderer packet groups observed through frame 360 decompose into 24 fixed device-initialization point draws from 0x8222B678 and six internal rectangle clears emitted below 0x8222BC18; they do not identify a missing normal Gears 1 draw wrapper.

## Evidence

Exact-revision literal DRAW_INDX/DRAW_INDX_2 writer census plus headless GEARS_NATIVE_RHI_OBSERVE provenance run through frame 360, recorded in issue #141 Renderer-input comparison

## What would falsify it

any unmatched packet group is attributed outside 0x8222B678 or the 0x8222BC18 clear chain, or a normal content draw reaches the renderer without one of the four semantic draw wrappers

## Re-confirmed 2026-08-30

Committed tree 1eb0f94 carries the exact-revision writer census result, terminal ring/indirect provenance, ambiguity controls, and issue #141 evidence for the 24 startup plus six internal-clear decomposition.

## Re-confirmed 2026-08-30

Committed c19182d frame-540 cadence report still shows exactly 30 cumulative unmatched groups: six materialized and 24 refused, all indirect, with no mixed outcome or source conflict; the exact writer ownership recorded in issue 141 is unchanged.
