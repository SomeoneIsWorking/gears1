---
id: 146
title: Suppressing redundant Vulkan graphics-state binds is below current resolution
status: dead-end
symptom: The renderer emits identical pipeline, dynamic-state, and index-buffer binds across many consecutive draws
tags: performance,renderer,vulkan,dead-end
created: 2026-08-27
updated: 2026-08-27
---

**Tried:** a frame-local command-state cache, suppressing a bind whose pipeline,
dynamic state, and index buffer were already set on the same submission.

**Result:** the mechanism worked (1,013 of 1,556 pipeline binds, 1,451 of 1,556
viewport/scissor/depth-bias groups, 883 of 1,454 index-buffer binds removed on
chapter45_recovered) and the frame time did not move: -0.51 ms against a 3.48 ms
floor, and -0.65 ms over 494/494 frames against a 1.13 ms floor. Uniform
descriptor sets stayed unique, so all 1,556 descriptor binds remained.

**Settling fact:** the saving is real and below this host's frame-time
resolution. The experiment and its controls were removed in full; do not
reintroduce state suppression as a performance claim without a workload or
measurement that resolves it. Descriptor-set ownership is the larger remaining
command-state obstacle.
