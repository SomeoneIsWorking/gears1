---
id: C102
kind: claim
status: holds
created: 2026-08-30
tags: native-rhi
depends: runtime/rhi_renderer_input.cpp#CompareRhiRendererDraws, runtime/gpu_draw.cpp#Renderer::RenderFrameImpl
reconfirmed: 2026-08-30
verified_at: 2026-08-30 03:15:07
---

## Claim

The actual compatibility-renderer materialization join correlated 293 semantic draws through Gears 1 frame 300 with zero value mismatches, zero unkeyed inputs, and zero duplicates; one replaced renderer frame was reported missing and 30 renderer packet groups remained unmatched.

## Evidence

2026-08-30 Clang headless GEARS_NATIVE_RHI_OBSERVE=1 run recorded in docs/issues/0141-native-rhi-lacks-a-grounded-per-draw-semantic-ob.md Renderer-input comparison

## What would falsify it

a current exact-revision headless run reports a semantic value mismatch, unkeyed renderer draw, duplicate publication, or a packet-address collision that does not represent tile replay

## Re-confirmed 2026-08-30

Final-tree Clang build, 90-test CTest suite, clang-format/clang-tidy gate, focused renderer-input negative controls, and headless GEARS_NATIVE_RHI_OBSERVE run reached frame 60 with 58 matches, zero missing/value mismatches/unkeyed inputs/duplicates, and 26 explicitly unmatched renderer packet groups; the same committed tree previously reached frame 300 with 293 matches, one explicit dropped-frame missing result, 30 unmatched renderer packet groups, and zero value mismatches.
