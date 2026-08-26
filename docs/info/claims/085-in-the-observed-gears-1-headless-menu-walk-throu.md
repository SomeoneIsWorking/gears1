---
id: C085
kind: claim
status: holds
created: 2026-08-27
tags: native-rhi,draw
depends: runtime/rhi_semantic_stream.cpp, runtime/titles/gears1/rhi_draw_bindings.cpp
---

## Claim

In the observed Gears 1 headless menu walk through frame 1712, every one of 90,854 calls through the three exercised normal draw bindings matched the DRAW_INDX packet emitted by its retained recompiled body.

## Evidence

scratch/logs/rhi-gameplay-observe.log aggregate recorded in docs/issues/0141-native-rhi-lacks-a-grounded-per-draw-semantic-ob.md; tests/test_rhi_semantic_stream.cpp negative control

## What would falsify it

Any same-run missing or mismatched packet from an exercised binding, evidence that the backward packet scan selected an older draw, or discovery of a normal draw entry point outside the grounded family.
