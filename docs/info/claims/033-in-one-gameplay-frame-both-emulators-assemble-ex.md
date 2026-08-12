---
id: C033
kind: claim
status: falsified
created: 2026-08-12
tags: 
falsified_on: 2026-08-12
---

## Claim

In one gameplay frame both emulators assemble exactly 54,352 primitives across the six draws of vertex shader f3e9368c1bb68ecc, but the console keeps 21,296 after clipping and we keep 235. The two 6,592-primitive draws clip to nothing on BOTH sides; the divergence is entirely in the four 10,292-primitive draws, where the console keeps about 52% and we keep 0 to 1.2%.

## Evidence

GEARS_ORACLE_PRIM_STATS pipeline-statistics query in the fork (9cfb478) against our GEARS_DRAW_DIAG table, scratch/vsord

## What would falsify it

a run in which our post-clip counts for the 10,292-primitive draws approach the console's, or one in which the assembled totals stop matching

## FALSIFIED 2026-08-12

Compared frames at different CAMERAS. The assembled counts matching (54,352 on both sides) says the same objects were submitted, not that the viewpoint was the same, and post-clip counts are almost entirely a function of viewpoint. Joining instead on the VIEW-PROJECTION ITSELF -- c230..c233, guest data present on both sides -- finds one of our frames within a max component delta of 3.94 of the console's, against a spread up to 1189 across 710 of our dumps. At that camera our counts are 5305/0/5325/5261/5220/0 = 21,111 of 54,352 against the console's 5362/0/5412/5254/5268/0 = 21,296 of 54,352, a 0.9% difference attributable to the residual camera offset. There is no clip divergence.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
