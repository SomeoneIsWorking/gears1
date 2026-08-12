---
id: C035
kind: claim
status: holds
created: 2026-08-12
tags: 
reconfirmed: 2026-08-12
verified_at: 2026-08-12 21:51:48
---

## Claim

Our shadow-atlas clear rasterises zero fragments, and only on that surface. All twelve clear draws targeting depth base 0x5a0 (vertex shader 760aacf6212e632c, depth func ALWAYS, depth write on) survive clipping and produce no fragment invocations, so the clear writes nothing and the atlas stays at far depth -- which is the whole of issue #97. The same shader produces 58,604 fragments across the other 57 draws of our frame, and the console produces 16,476 across its 59, so neither the shader nor our translation of it is at fault.

## Evidence

scratch/camgate/match/draws.tsv against GEARS_ORACLE_PRIM_STATS=760aacf6212e632c on the fork

## What would falsify it

a capture in which those twelve draws produce fragments, or one in which the console's equivalent draws also produce none

## Re-confirmed 2026-08-12

Re-measured after the depth-scale fix (C050), which changed depth values across the frame and so could plausibly have moved this: it did not. scratch/depthfix/draws.tsv, camera-gated capture matched at 0.13 thresholds. Vertex shader 760aacf6212e632c runs 67 draws in the frame; the 8 targeting depth base 0x5a0 survive clipping (prims 1->2, 3->6, 2->4) and produce ZERO fragment invocations, verdict rasterised_no_fragment on every one, while the same shader produces 58,604 fragments across all 67. So the atlas clear still writes nothing and issue #97's symptom is independent of the depth scale. The draw count differs from the original 12 because this is a different game moment; the zero is what reproduces, not the count. Note runtime/gpu_draw_probe.cpp is the source of the fragment counts (VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT, :1170), which the original evidence did not name.
