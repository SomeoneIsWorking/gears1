---
id: C104
kind: claim
status: holds
created: 2026-08-30
tags: native-rhi,renderer-parity,target-state
depends: runtime/rhi_target_state.h#DecodeRhiColorTargetDescriptor, runtime/rhi_semantic_state.cpp#RhiSemanticStateTracker::ApplyColorWriteState, runtime/rhi_renderer_input.cpp#InspectRhiRendererDrawInput, runtime/titles/gears1/color_write_gamma_override.cpp#sub_82229B28, runtime/titles/gears1/rhi_bindings.cpp#CaptureColorRenderTargetBinding, runtime/titles/gears1/rhi_bindings.cpp#CaptureDepthStencilTargetBinding
---

## Claim

Gears 1 normalized active RT0/depth/surface state, including ordered post-bind color-write mutations, agrees with terminal compatibility NativeDrawInput for every correlated draw through the frame-660 gameplay transition

## Evidence

Clang headless GEARS_NATIVE_RHI_OBSERVE=1 plus GEARS_LUCENT_DEBUG=rhi run in scratch/logs/rhi-target-long-alias.log: frame 660 reports 10,945 semantic matches, zero missing and zero value mismatches; semantic totals report 15,306 matching color-write transitions, 2,854 color-target binds, and 1,566 depth-target binds. Focused tests mutate every normalized target field, missing state, unsupported MRT slots, and physical index address.

## What would falsify it

Any correlated materialized draw reports a target-state missing/mismatch reason, any post-bind color-write transition lacks or disagrees with its active slot-zero target, or an independently decoded target field differs after physical address normalization.
