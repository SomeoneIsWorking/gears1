---
id: C026
kind: claim
status: falsified
created: 2026-08-07
tags: 
falsified_on: 2026-08-11
---

## Claim

At a gameplay moment 300 frames past the level-start fade, our presented buffer agrees with the console per channel; our renderer executes 13 of that frame's 19 copy draws and never serves the 5 DEPTH resolves

## Evidence

tools/layer_capture.sh 700 scratch/layercap3 -- ours guest frame 3223, console 3234, both selected by 'the frame 300 after the first with >= 400 draws'. Final copy (ours 0x311000, theirs 0x1F606000, k_8_8_8_8, both read off the GPU right after the copy): ours R 0.0181 G 0.0279 B 0.0301, theirs R 0.0179 G 0.0282 B 0.0292, 10.0% of pixels differ by >0.05 (camera motion over the 11-frame offset). Our log: 'per-resolve snapshots: 13 captured, of 13 resolves this renderer EXECUTED and 19 copy draws the frame CONTAINS' and 'frame resolves not served: 5 from depth (no host depth texture chain yet)'. See catalog #90 and instrument I031.

## What would falsify it

a paired capture at the same offset whose presented-buffer channel means diverge by more than 0.005, or the not-served depth count moving off 5

## FALSIFIED 2026-08-11

The depth half is WRONG. The 5 'never served' depth copies were EXECUTED all along: ResolveDepthTo has dispatched them since 2026-07-28 ('Serve the resolved depth, and the light shafts appear'), and the execute-time counter in the SAME log said 'frame depth resolves: 2 executed, 0 skipped'. Two instrument defects made them invisible: (1) PlanResolves counted every depth copy as unserved and printed 'no host depth texture chain yet', text carried over from before the depth chain existed; (2) the per-resolve snapshot was taken only in the colour branch, so a depth copy that ran produced no file and read as a pass the renderer skipped. The presented-buffer half of the claim is unaffected and is restated in the replacement claim.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
