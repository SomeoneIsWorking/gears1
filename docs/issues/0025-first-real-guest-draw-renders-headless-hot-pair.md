---
id: 25
title: First real guest draw renders headless; hot pair is an RT-sampling post-process so output is black over a stub RT
status: resolved
symptom: need the hot pair full-screen quad drawn with its translated SPIR-V into the swapchain and presented; whether a real draw could be produced headless was unproven
tags: gpu,draw,vulkan,shaders,draw-backend,rt,stub
created: 2026-07-23
updated: 2026-07-23
---

runtime/gpu_draw.cpp draws the hot pair (vs_5363d074/ps_501ac5d8) headless (GEARS_DRAW=1) into a 1280x720 offscreen target, screenshot scratch/screenshots/hot_draw.ppm.

ARCHITECTURE: gpu_draw_xlate.cpp (Xenia Xenos->SPIR-V + SystemConstants derivation, in a static lib linked PRIVATE so xenia's bundled Vulkan-Headers / VK_NO_PROTOTYPES do not collide with system Vulkan in gpu_draw.cpp/gpu_present.cpp). gpu_draw.cpp: own headless Vulkan 1.2 device; SSBO = verbatim mirror of low guest phys memory; UBOs set1 b0 system / b1 float-vertex / b2 float-pixel / b3 bool-loop / b4 fetch; float UBOs packed per Xenia constant_register_map (VS c0-c3 from 0x4000, PS c0/c1/c255 from 0x4400); 1x1 stub texture0 (2D array) + sampler at set3; index buffer from guest 0x978d0; pipeline from the 2 runtime-translated SPIR-V modules (no Vulkan vertex input -- vfetch reads the SSBO). vd_null_gpu.cpp mirrors VGT_DRAW_INITIATOR (reg 0x21FC) into the register file at DRAW_INDX and fires the backend once at the hot pair. gpu_present.cpp uploads the frame into the swapchain.

PROVEN: runtime translation == verified .spv byte-for-byte (VS 12420, PS 18588); the 4 SSBO vertices at phys 0x97810 are the real NDC full-screen quad; magenta-sentinel clear fully overwritten (921600/921600 px) so the quad rasterised (triangle_list, 6 int32 indices 0 1 3 0 3 2) and the PS ran over every pixel; Vulkan validation clean.

RESULT: black frame. ROOT CAUSE (verified, not assumed): this hot pair is a full-screen RT-sampling post-process; texture0 is a 1x1 stub so the sample returns (0,0,0,0) (alpha 0 confirms the sample itself is zero) and the PS log()/exp() path collapses to black. Isolation: no-draw run shows the magenta clear (clear/copy/readback correct); identity-swizzle diagnostic still black (so it is the stub sample, not swizzle, and not a UBO/SSBO bug -- VS position is correct). fps steady-state ~30 with the draw active == headless baseline (draw is one-shot).

NEXT PREREQ (re-frontier draw-backend-rt): render the scene into the RT this pair samples (bind it as texture0), or target a self-contained hot pair, for a console-matching frame.
