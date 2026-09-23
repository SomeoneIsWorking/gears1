---
id: 173
title: Product startup failed twice during a concurrent heavy build
status: resolved
symptom: two offscreen runs never presented, one hung with every guest thread idle after thread 13 started, one aborted with "double free or corruption (!prev)" just after Xenia loaded stored pipeline descriptions
tags: stability,xenia,shader-storage,startup
state_items: S009
created: 2026-09-22
updated: 2026-09-23
---

## Cause

Two data races in Xenia's parallel translation of stored shaders at startup
(`VulkanPipelineCache::TranslateShadersForStorage`), which runs on every
launch with a populated storage root (381 translations for the current Gears
1 root). Neither depends on a concurrent build; the build only coincided.

- Different shaders translated on different threads looked up and grew the
  shared `texture_binding_layout_map_` and `texture_binding_layouts_` with no
  lock. `layouts_mutex_` was declared for them and never taken; the D3D12
  pipeline cache takes its counterpart.
- Two modifications of one shader translated on different threads could read
  the shader's texture and sampler bindings for binding-layout setup while the
  other thread was still writing them.

## Evidence

- A core dump of a third failure, on an otherwise idle host, aborted with
  "corrupted size vs. prev_size" in a shader translation thread, while two
  more translation threads were inside `SpirvShaderTranslator`.
- A hang reproduced at 6,257 translated functions with the unfixed build logs
  "381 shader translations needed" and never "Translated N shaders": the
  storage translation never finished.
- The failure is rare: 1 of 30 and 0 of 45 unfixed startups failed, so a
  clean loop is weak evidence by itself. The fixed build started cleanly 30 of
  30 times, each translating all 381 shaders.

## Fix

Fork commits f36404b (bindings published once with `std::call_once`, for both
the SPIR-V and DXBC translators) and efee73e (the layout maps are taken under
`layouts_mutex_`).
