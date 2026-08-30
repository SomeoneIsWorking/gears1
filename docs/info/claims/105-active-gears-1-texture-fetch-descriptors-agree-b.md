---
id: C105
kind: claim
status: holds
created: 2026-08-30
tags: native-rhi,texture
depends: runtime/rhi_renderer_input.cpp#InspectRhiRendererDrawInput, runtime/rhi_semantic_state.cpp#RhiSemanticStateTracker::ApplyBinding, runtime/gpu_draw_native_input.cpp#BuildNativeDrawInput, runtime/titles/gears1/rhi_bindings.cpp#ObserveTextureStateSetter, runtime/titles/gears1/rhi_bindings.cpp#sub_8254E9E0, runtime/titles/gears1/shader_setter_override.cpp#CaptureShaderBinding
---

## Claim

Active Gears 1 texture fetch descriptors agree between the ordered title semantic state and the compatibility renderer's terminal PM4-derived NativeDrawInput through frame 1607.

## Evidence

A final uninstrumented Clang headless run matched 118,553 correlated semantic draws with zero missing or value-mismatched renderer inputs after modeling retained setters 0x8222A150, 0x8222A2D8, 0x8222A550, and 0x8254E9E0. The earlier arms produced 66,429 and 325 mismatches, and focused tests reject all six dword mutations plus missing, duplicate, and unsupported slots.

## What would falsify it

Any active slot reports a missing or mismatched six-dword descriptor, a scoped watch finds another unmodeled descriptor writer, or the renderer projection stops deriving its state independently from the PM4 register file.
