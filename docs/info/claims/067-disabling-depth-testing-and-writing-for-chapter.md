---
id: C067
kind: claim
status: holds
created: 2026-08-14
tags: render,oracle,first-divergence
depends: tools/gfr_to_xtr.py, extern/xenia/src/xenia/gpu/vulkan/vulkan_pipeline_cache.cc
---

## Claim

Disabling depth testing and writing for chapter-45 draw 743 on the Xenia checkpoint does not make its C400 resolve nonzero

## Evidence

scratch/ch45_prefix_probe/checkpoint_nodepth.xtr replay: IssueCopy wrote 5,242,880 bytes and the raw C400 destination contained zero nonzero bytes

## What would falsify it

A same-prefix draw-743 oracle replay with depth disabled produces any nonzero C400 byte
