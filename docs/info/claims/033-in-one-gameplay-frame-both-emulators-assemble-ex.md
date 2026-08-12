---
id: C033
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

In one gameplay frame both emulators assemble exactly 54,352 primitives across the six draws of vertex shader f3e9368c1bb68ecc, but the console keeps 21,296 after clipping and we keep 235. The two 6,592-primitive draws clip to nothing on BOTH sides; the divergence is entirely in the four 10,292-primitive draws, where the console keeps about 52% and we keep 0 to 1.2%.

## Evidence

GEARS_ORACLE_PRIM_STATS pipeline-statistics query in the fork (9cfb478) against our GEARS_DRAW_DIAG table, scratch/vsord

## What would falsify it

a run in which our post-clip counts for the 10,292-primitive draws approach the console's, or one in which the assembled totals stop matching
