---
id: C044
kind: claim
status: holds
created: 2026-08-12
tags: render,depth,resolve
depends: runtime/gpu_draw_resolve_decode.cpp
---

## Claim

Giving resolve entries their own RB_DEPTH_INFO base changes the shadow-atlas depth resolve from near-empty to near-full under GEARS_DRAW_SPLIT_DEPTH=1 -- an A/B on our own side, needing no cross-console pairing.

## Evidence

scratch/layercap_fix2: srcD5A0 864x864 f22 #0 and #1 read 0.0209 and 0.0206 on the build BEFORE the fix and 0.7095 and 0.8701 after it. Our side against our side, so no pairing is involved and C043's 0.49 pairing limit does not touch it; a two-orders-of-magnitude change is far beyond moment-to-moment variation. The console's values (0.7082, 0.8754) sit in the same range, which is CONSISTENT with the fix being right but is not evidence of the PRECISION of that agreement -- that needs a pair passing the same-picture gate.

## What would falsify it

a build in which reverting the RB_DEPTH_INFO change still leaves those passes near 0.7, which would mean something else produced the jump
