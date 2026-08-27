---
id: C093
kind: claim
status: holds
created: 2026-08-28
tags: native-rhi,render-target,gears1
depends: runtime/rhi_semantic_state.cpp#RhiSemanticStateTracker, runtime/titles/gears1/rhi_bindings.cpp#CaptureBoundRenderTargets, runtime/rhi_semantic_stream.cpp#CompareRhiDrawRenderTargetState
---

## Claim

Gears 1 draw calls consume ordered active color/depth target object identities from the target binders, while target descriptors may change after binding and require separate render-state ownership

## Evidence

The semantic state tracker applied 0x8222B068 color and 0x8222B398 depth bindings, while the post-draw adapter independently read four color object slots and the depth slot. A headless run through frame 600 matched all 3,786 draw snapshots, including 482 bound-index draws, with no missing or mismatched observations. A deliberately over-scoped first arm persisted bind-time descriptors and produced 3,141 mismatches by frame 630: object identity remained equal while color descriptor 0x302D0 became 0xC02D0 without another target bind.

## What would falsify it

Falsified if a target object changes at draw time without the corresponding binder event, ordered object snapshots diverge from the device slots, or a grounded descriptor-state setter proves the descriptor is immutable binding state.
