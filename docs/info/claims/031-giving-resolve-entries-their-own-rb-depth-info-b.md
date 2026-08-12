---
id: C031
kind: claim
status: holds
created: 2026-08-12
tags: render,depth,resolve,oracle
depends: runtime/gpu_draw_resolve_decode.cpp, tools/layer_capture.sh
---

## Claim

Giving resolve entries their own RB_DEPTH_INFO base fixes the shadow-atlas depth resolve under GEARS_DRAW_SPLIT_DEPTH=1, verified against the console

## Evidence

Paired capture scratch/layercap_fix2, ours guest frame 873 / console 873, gap 0, 16 of 16 passes shared with zero one-sided. srcD5A0 864x864 f22 #0: ours 0.7095 against the console's 0.7082, |d| 0.023 over what the console wrote. #1: ours 0.8701 against 0.8754, reported a MATCH at |d| 0.0168. On the build before the fix the same two passes read 0.0209 and 0.0206 against the same console values.

## What would falsify it

a later paired capture in which those passes diverge again, or a demonstration that layer_compare's depth pairing is decoding one side wrongly
