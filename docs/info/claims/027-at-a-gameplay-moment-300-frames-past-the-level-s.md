---
id: C027
kind: claim
status: holds
created: 2026-08-11
tags: resolve,oracle
depends: runtime/gpu_draw_resolve_plan.cpp, runtime/gpu_draw.cpp
---

## Claim

At a gameplay moment 300 frames past the level-start fade, our renderer executes 16 of the frame's 18 copy draws including all 3 DEPTH copies, and the depth passes pair with the console's once they are keyed by RB_DEPTH_INFO

## Evidence

tools/layer_capture.sh 420 scratch/layercap_depth2 -- ours guest frame 3219, console 3222, same content selector. layer_compare: 'passes both sides resolve: 12' (was 9), with srcD000 1280x720 f23 #0 and srcD5A0 864x864 f22 #0/#1 now paired on BOTH sides. Runtime log: 'frame depth resolves: 2 executed (1 float24/kD24FS8, 1 unorm24/kD24S8), 0 skipped' and 'frame depth copies routed to a host destination: 3; NOT routed: 0'. The 2 copy draws we still do not execute are the second tile of srcD000 (1280x208) and srcC400 1280x208, both dropped by the predicated-tile collapse by design. Depth pass VALUES are still not compared (catalog #35): our destination holds decoded float depth, the console's holds packed guest bytes.

## What would falsify it

a paired capture in which a depth key present on the console has no counterpart on our side, or in which the executed-copy count falls below 16 of 18
