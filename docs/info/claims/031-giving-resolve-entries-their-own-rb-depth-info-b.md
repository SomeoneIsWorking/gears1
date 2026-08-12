---
id: C031
kind: claim
status: falsified
created: 2026-08-12
tags: render,depth,resolve,oracle
depends: runtime/gpu_draw_resolve_decode.cpp, tools/layer_capture.sh
falsified_on: 2026-08-12
---

## Claim

Giving resolve entries their own RB_DEPTH_INFO base fixes the shadow-atlas depth resolve under GEARS_DRAW_SPLIT_DEPTH=1, verified against the console

## Evidence

Paired capture scratch/layercap_fix2, ours guest frame 873 / console 873, gap 0, 16 of 16 passes shared with zero one-sided. srcD5A0 864x864 f22 #0: ours 0.7095 against the console's 0.7082, |d| 0.023 over what the console wrote. #1: ours 0.8701 against 0.8754, reported a MATCH at |d| 0.0168. On the build before the fix the same two passes read 0.0209 and 0.0206 against the same console values.

## What would falsify it

a later paired capture in which those passes diverge again, or a demonstration that layer_compare's depth pairing is decoding one side wrongly

## FALSIFIED 2026-08-12

OVERSTATED: 'verified against the console' is not supported by the evidence cited, though the FIX ITSELF is real and is not in question. The claim rests on mean-based layer_compare verdicts -- 'ours 0.7095 against the console's 0.7082, |d| 0.023' and 'a MATCH at |d| 0.0168' -- and C043 establishes that the content selector both sides use pairs them to a log-luminance correlation of only 0.49, against 0.94 for a genuine match. A MEAN agrees between two different moments of the same scene, so agreement to |d| 0.023 does not show the two sides rendered the same thing; it is consistent with that and with a moment mismatch. The evidence also cites 'ours guest frame 873 / console 873, gap 0', which is pairing by FRAME INDEX -- the exact hazard docs/codemap.md warns against, since both emulators advance by wall-clock delta (#84/#98). WHAT SURVIVES, re-filed separately: the A/B on our own side. The same two passes read 0.0209 and 0.0206 before the fix and 0.7095 and 0.8701 after it. A change from 0.02 to 0.71 is two orders of magnitude beyond any moment-to-moment variation, so the fix plainly does what it was built to do -- that needs no pairing at all, being our side before against our side after. What is NOT established is the PRECISION of the agreement with the console.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
