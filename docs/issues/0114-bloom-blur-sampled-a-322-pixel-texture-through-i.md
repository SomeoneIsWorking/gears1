---
id: 114
title: Bloom blur sampled a 322-pixel texture through its 352-pixel guest pitch
status: resolved
symptom: The first bloom resolve matched Xenia, but the next blur lost the upper-right glow and the downstream composite had 4.98% of pixels differ by more than 0.1
tags: gpu,bloom,resolve,texture,pitch,extent,gameplay
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

RB_COPY_DEST_PITCH is guest-memory row storage. The bloom consumer fetch constant declares a logical 322x182 texture at pitch 352. The native resolve cache created a 352x182 Vulkan image, so normalized coordinates were divided by 352 and every blur sample landed too far left. The first bright-pass resolve was close because it only wrote the image; the first consumer blur was the first divergent pass.

## Fix and falsifiers

runtime/gpu_resolve_extent.cpp derives the unique logical consumer extent before resolve-image creation; gpu_draw_targets.cpp uses it for the sampled VkImage while retaining guest pitch and height for routing and dump identity. Conflicting aliases warn and retain the storage extent rather than guessing. test_gpu_resolve_extent drives 352 pitch / 322 logical, ordinary, absent and conflicting cases. layer_compare self-test fills pitch padding with deliberately different pixels and proves it is untiled but excluded from content.

On chapter45_recovered.gfr against the synchronous Xenia oracle, all three C5A0 passes now compare over 322x182 with 0.00% of pixels differing by more than 0.1. The downstream C2D0 composite fell from 4.98% to 0.41%, and the final resolve has 0.00%. A fresh full replay with per-resolve dumps is Vulkan-validation clean.
