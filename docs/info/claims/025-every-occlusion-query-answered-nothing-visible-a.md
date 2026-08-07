---
id: C025
kind: claim
status: holds
created: 2026-08-07
tags: 
---

## Claim

Every occlusion query answered 'nothing visible', and that suppressed the title screen's post-processing pass group on 98% of frames

## Evidence

runtime/vd_null_gpu.cpp EVENT_WRITE_ZPD zero-filled the whole xe_gpu_depth_sample_counts record; D3D reads the result as END.ZPass - BEGIN.ZPass, so all-zero reports 'no pixels passed'. A/B on the same screen, same frame-keyed walk: GEARS_GPU_ZPD_ZERO=1 renders the 7-pair post group on 11 of 562 title frames (2.0%), the default monotonic-ZPass answer on 563 of 563 (100.0%). Draws per frame median 161 -> 171 against the console's 173; title mean red 0.0863 -> 0.1571 (console 0.2508); R/G 1.79 -> 2.68 (console 3.52). Also explains the burst pattern: the first title frame renders because no query has reported yet.

## What would falsify it

if a real Vulkan occlusion query pool (the proper fix this stopgap stands in for) reports genuinely-zero samples for these draws, the title would be right to cull them and the cause would be upstream in what we rasterise. Also falsified if the 2.0%-vs-100.0% split fails to reproduce on a fresh pair of runs.
