---
id: C034
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

Our shadow-volume clipping AGREES with the console. At matched cameras the six draws of vertex shader f3e9368c1bb68ecc assemble 54,352 primitives on both sides and keep 21,111 (ours) against 21,296 (console), per draw 5305/0/5325/5261/5220/0 against 5362/0/5412/5254/5268/0. Post-clip counts are almost entirely a function of the viewpoint, so any comparison of them must be joined on the VIEW-PROJECTION (c230..c233), which is guest data both emulators carry -- not on frame index, draw ordinal, or a content predicate.

## Evidence

scratch/clipmath/draws.tsv held frame (camera within 3.94 of the console's, out of a spread up to 1189 across 710 dumps) against GEARS_ORACLE_PRIM_STATS in scratch/vsord

## What would falsify it

a camera-matched pair in which the post-clip totals differ by more than a few percent
