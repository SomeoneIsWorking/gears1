---
id: C102
kind: claim
status: holds
created: 2026-08-30
tags: native-rhi
depends: runtime/gpu_draw_native_input.cpp#NativeFrameMaterializationRecorder, runtime/rhi_renderer_input.cpp#CompareRhiRendererDraws, runtime/render_thread.cpp#RenderThreadMain
reconfirmed: 2026-08-30
verified_at: 2026-08-30 05:10:13
---

## Claim

The actual compatibility-renderer materialization join correlated 293 semantic draws through Gears 1 frame 300 with zero value mismatches, zero unkeyed inputs, and zero duplicates; one replaced renderer frame was reported missing and 30 renderer packet groups remained unmatched.

## Evidence

2026-08-30 Clang headless GEARS_NATIVE_RHI_OBSERVE=1 run recorded in docs/issues/0141-native-rhi-lacks-a-grounded-per-draw-semantic-ob.md Renderer-input comparison

## What would falsify it

a current exact-revision headless run reports a semantic value mismatch, unkeyed renderer draw, duplicate publication, or a packet-address collision that does not represent tile replay

## Re-confirmed 2026-08-30

Final-tree Clang build, 90-test CTest suite, clang-format/clang-tidy gate, focused renderer-input negative controls, and headless GEARS_NATIVE_RHI_OBSERVE run reached frame 60 with 58 matches, zero missing/value mismatches/unkeyed inputs/duplicates, and 26 explicitly unmatched renderer packet groups; the same committed tree previously reached frame 300 with 293 matches, one explicit dropped-frame missing result, 30 unmatched renderer packet groups, and zero value mismatches.

## Re-confirmed 2026-08-30

Committed Clang source tree 1eb0f94 passed the full build, 90 non-cpp_quality tests, repository-wide quality gate, final focused lint/tests, and current headless observation through frame 124 with zero value mismatches, unkeyed draws, duplicates, mixed outcomes, or source conflicts.

## Re-confirmed 2026-08-30

Committed c19182d frame-660 headless materialization join correlated 10,945 semantic draws with zero missing, value mismatches, unkeyed inputs, or duplicates before any queue drop; focused arrival, duplicate, tile-replay, alias, and field-mutation controls pass.
