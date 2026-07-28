---
id: 36
title: Cull mode was host-fixed to NONE, so every back face was rasterised
status: resolved
symptom: the rasteriser used VK_CULL_MODE_NONE for every draw regardless of PA_SU_SC_MODE_CNTL; 380 of the frame's world draws ask for cull_back
tags: gpu,draw,draw-backend,cull,raster,gameplay
created: 2026-07-28
updated: 2026-07-28
---

FIXED and VERIFIED. Culling is now the guest's own, per Xenia's
VulkanPipelineCache::GetCurrentStateDescription: cull_front (PA_SU_SC_MODE_CNTL
bit 0) and cull_back (bit 1) become the Vulkan cull bits, and face (bit 2)
selects the winding -- gated on the primitive being POLYGONAL, since faceness is
meaningless for points and lines and applying it to them would drop geometry the
hardware keeps. The state joins the pipeline key.

THE ONE THING THE REGISTER DOES NOT SETTLE is the winding, because our Y flip
lives in the shader's ndc_scale and a Y flip reverses screen-space winding. So
that was measured rather than reasoned about, with GEARS_DRAW_CULL_INVERT as the
control arm and GEARS_DRAW_NOCULL as the baseline:

                prims in   after clip+cull        fragment invocations
    no cull       52725      7421 (14.1%)                   9,129,959
    cull          52725      3652  (6.9%)                   9,273,329
    cull inverted 52725      4346  (8.2%)                   6,008,100

The register-derived winding HALVES the surviving primitives, which is the
signature of correct backface culling on closed geometry, and fragment
invocations go UP slightly -- front faces that previously lost the depth test to
a back face now win it. Inverting the winding survives MORE primitives (4346) and
shades far fewer fragments (6.0M), which is culling the front faces instead: the
draws that run the fragment shader drop from 52 to 37 while no-cull has 53.

So no Y-flip compensation is needed, and Xenia's direct reading of `face` is
right. Frame diff against no-cull: 17692 of 2764816 bytes. Vulkan validation
clean.

Worth noting why this was never a visible bug: cull mode NONE can only keep MORE
primitives than the guest asked for, so nothing went missing -- back faces were
simply drawn, mostly hidden behind their own front faces. It cost fill rate, and
it would have been visibly wrong wherever the guest blends.
