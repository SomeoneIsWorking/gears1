---
id: 141
title: Native RHI lacks a grounded per-draw semantic observation seam
status: investigating
symptom: Per-draw semantics are now observed and packet-checked, but complete state, resource, resolve, presentation, and retirement semantics are not yet mirrored, so native execution cannot safely bypass PM4
tags: performance,native-rhi,d3d,re,draw,seam
created: 2026-08-27
updated: 2026-08-27
---

## Root cause

The first search assumed one title draw call should equal one compatibility-renderer draw execution. That assumption is false: the title emits one logical draw packet, then the Xenos command stream replays predicated packets per EDRAM tile. The inflated PM4 execution count hid the real normal draw family, while the prior static candidate `sub_82544148` was only a once-per-frame state operation.

## Resolution so far

A scoped alias-aware guest-memory write watch identified the packet producer
chain. Raw packet-construction scans and direct disassembly then grounded the
four normal entry points: `0x8222CFF8` (transient vertices), `0x8222D4F8`
(transient vertices and indices), `0x8222DA48` (bound vertices), and
`0x8222DE50` (bound indices).

The exact Gears 1 bindings in `runtime/titles/gears1/rhi_draw_bindings.cpp`
retain and super-call every recompiled body. Only when
`GEARS_NATIVE_RHI_OBSERVE=1` is enabled, they publish ordered typed draws to the
title-neutral `runtime/rhi_semantic_stream.*`, which validates primitive type,
element count, and auto-index versus DMA source against the packet emitted by
the original body. A headless menu walk through frame 1712 observed 90,854
calls and produced 90,854 matches, zero missing packets, and zero mismatches:
17,864 transient-vertex, 60,465 transient-vertex-and-index, and 12,525
bound-index calls. The bound-vertex path remains statically grounded but was not
reached by that walk. A focused negative control mutates the packet evidence and
is rejected as a mismatch.

## Next falsifier

Exercise the bound-vertex path dynamically, then mirror the complete ordered
state/resource/draw/resolve/present stream and compare it with the PM4-derived
`FrameDrawInputs` and output. Keep the recompiled compatibility bodies available
and super-called until a deliberately wrong semantic control is rejected and a
same-run complete-stream and pixel-parity gate agrees. Per-draw packet agreement
alone does not authorize a native bypass.
