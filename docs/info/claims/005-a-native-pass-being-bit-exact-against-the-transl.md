---
id: C005
kind: claim
status: holds
created: 2026-08-05
tags: gpu,native-renderer,method
depends: tools/verify_native_pass.sh, runtime/shaders/uber_post_blend.frag
---

## Claim

A native pass being bit-exact against the translated shader does NOT establish that it implements the translator's interface; only Vulkan validation can

## Evidence

runtime/shaders/movie_yuv.frag and scene_gamma.frag were both verified bit-exact (2,764,800 of 2,764,800 channel samples, twice each, with negative controls) while declaring their sampled images as texture2D against descriptors this renderer binds as VK_IMAGE_VIEW_TYPE_2D_ARRAY (gpu_draw.cpp:1643 and :2232). The driver tolerates the mismatch and samples layer 0, so the pixel comparison could not see it. GEARS_DRAW_VALIDATE=1 reports it per draw: 'the sampled image descriptor [Set 3, Binding 0] VkImageViewType is VK_IMAGE_VIEW_TYPE_2D_ARRAY but the OpTypeImage has (Dim = 2D) and (Arrayed = 0)'. The same audit found two further interface errors in all three shaders -- texture size taken from textureSize() instead of the guest's fetch constant, and implicit-LOD sampling instead of explicit coarse gradients scaled by the fetch constant's LOD bias -- and the gradient one was the cause of 20 off-by-one samples in the third pass that no amount of arithmetic review located. After correction all three are still bit-exact and validation reports zero view-type warnings.

## What would falsify it

a native pass that passes GEARS_DRAW_VALIDATE=1 cleanly and still diverges from the translated shader -- that would mean validation is not sufficient either, and the next audit needs the dumped module compared structurally rather than a warning count
