---
id: 75
title: The renderer emitted invalid SPIR-V and cleared depth without TRANSFER_DST
status: resolved
symptom: GEARS_DRAW_VALIDATE=1 reports five distinct VUIDs, where the docs claim only the point-list PointSize warning
tags: gpu,vulkan,validation,spirv,resolve,depth
created: 2026-08-05
updated: 2026-08-05
---

## What was observed

Turning on `GEARS_DRAW_VALIDATE=1` on a frame_replay of act1_v2 printed five distinct VUIDs. `docs/re-frontier.md` (draw-backend-live) says 'Vulkan validation clean apart from the pre-existing point-list PointSize warning'. That claim was STALE, not wrong-at-the-time-unknown: nothing in the routine gates ever runs the validator, so the drift was invisible.

## Root causes -- two, both 'written to a later contract than the one declared'

**1. The shared depth image lacked VK_IMAGE_USAGE_TRANSFER_DST_BIT** (3 of the 5 VUIDs: vkCmdClearDepthStencilImage-pRanges-02660/02659, VkImageMemoryBarrier-oldLayout-01213). It was created SAMPLED|DEPTH_STENCIL_ATTACHMENT, but the guest's own mid-frame depth clear is a `vkCmdClearDepthStencilImage`, which requires TRANSFER_DST. The clear and its two layout barriers were undefined behaviour this driver happened to execute -- and that clear is what makes reverse-Z draws pass the depth test (see #31). Fixed in gpu_draw.cpp at image creation.

**2. The two resolve compute shaders declare Spv_1_0 but were built to SPIR-V 1.4 rules**, in two independent places (VkShaderModuleCreateInfo-pCode-08737):
- They listed their UniformConstant image variables and PushConstant block in `OpEntryPoint`. SPIR-V 1.4 widened the entry-point interface to every global variable; 1.0-1.3 allow only Input and Output.
- The `copy_dest_swap` `OpSelect` used a SCALAR bool condition on a `v4float` result. Legal from 1.4; earlier versions need a bool4, so the condition is now broadcast with OpCompositeConstruct.

Fixed at the EMISSION in gpu_draw_xlate.cpp, not by raising the declared SPIR-V version -- that would raise the Vulkan requirement to accommodate a bug.

The second error was MASKED by the first: spirv-val stops at the first failure per module, so the OpSelect only appeared once the entry-point interface was fixed. Expect more if any of this is touched again -- re-run the validator rather than assuming one fix cleared it.

## How the fix was verified

All 8 captures hash identically via tools/frame_hashes.sh. That does NOT cover the swap path on its own, so it was shown to be LIVE: with `GEARS_DRAW_RESOLVE_NOSWAP=1` the frame changes on act1_v2, courtyard and bright, so the rewritten OpSelect actually executes in all three and produces the same bytes as the invalid version did.

## The workflow lesson

`tools/frame_hashes.sh` sets no GEARS_DRAW_* knob, so the validator, the probes and every dump are dead code in the routine gate (see instrument I006). 'Validation clean' is only true as of the last run of the validator -- cite the run, not the memory.
