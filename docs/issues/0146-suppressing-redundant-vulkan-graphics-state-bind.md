---
id: 146
title: Suppressing redundant Vulkan graphics-state binds is below current resolution
status: dead-end
symptom: The renderer emits identical pipeline, dynamic-state, and index-buffer binds across many consecutive draws
tags: performance,renderer,vulkan,dead-end
created: 2026-08-27
updated: 2026-08-27
---

A frame-local command-state cache eliminated 1,013 of 1,556 pipeline binds, 1,451 of 1,556 viewport/scissor/depth-bias groups, and 883 of 1,454 index-buffer binds on chapter45_recovered; uniform descriptor sets remained unique and therefore all 1,556 descriptor binds remained.

The mechanism was correct but the performance result was not established. A 201-render interleaved A/B measured -0.51 ms against a 3.48 ms floor. A 1,001-render run measured -0.65 ms over 494/494 frames against a 1.13 ms floor: NOT RESOLVED. The experiment and its controls were removed in full. Do not reintroduce state suppression as a performance claim without a workload or instrument that resolves it; descriptor-set ownership is the larger remaining command-state obstacle.
