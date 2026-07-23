---
id: 26
title: Whole-frame draw backend works, but only the loading frame is reachable: guest quits via XamLoaderLaunchTitle before any scene frame
status: open
symptom: generalised backend renders every draw of a frame, but the frame contains only 24 points + 2 rectangles and shades black; no post-load scene geometry ever executes
tags: gpu,draw,draw-backend,frame,vulkan,boot,xam
created: 2026-07-23
updated: 2026-07-23
---

The guest-draw backend was generalised from the hot-pair one-shot to a whole-frame renderer (runtime/gpu_draw.cpp RenderFrame, wired from runtime/vd_null_gpu.cpp CaptureFrameDraw/TriggerFrameRender, GEARS_DRAW_FRAME=1).

WHAT WORKS (measured headless, GEARS_NO_WINDOW=1, Vulkan validation on):
- 26 of 26 DRAW_INDX_2 issued, 0 skipped, in submission order, into ONE persistent colour+depth target inside a single render pass.
- 2 distinct shader pairs, 3 distinct shaders, 3 pipelines. Each distinct shader is translated once (Xenos->SPIR-V) and cached by hash; pipelines cached per (vs,ps,prim).
- Each draw carries its OWN register-file snapshot, so the constants/fetch/initiator live at that draw feed its UBOs -- not the state at frame end.
- Both source paths issue: kDMA (index buffer read from guest memory, endian-swapped) and kAutoIndex (vkCmdDraw, gl_VertexIndex 0..n-1).
- Real guest geometry rasterises: the rectangle-list draw yields a visible triangle whose vertices come from the guest's own memory through the SSBO vfetch (scratch/screenshots/frame_crop.png).

WHAT THE FRAME ACTUALLY IS (the honest limit): the only frame reachable is the LOADING phase --
24 point-list draws of 1 vertex each (prim 0x1) and 2 rectangle-list draws of 3 vertices (prim 0x8),
ALL kAutoIndex, confined to a 200x62 region in the top-left, 6328/921600 px (0.7%), shading pure BLACK
over the 1x1 stub texture0 (the same texture gap that makes the hot pair black, catalog #25).
That is not the 'accumulated scene geometry' bar.

WHY NO SCENE FRAME (the blocking prerequisite, NOT a draw-path defect): immediately after this first
frame the guest calls XamLoaderLaunchTitle with r3=0 (NULL path). Xenia's semantics for a null path is
'exit to dashboard' (kernel_state()->TerminateTitle(); the function does not return), so the title is
deciding to QUIT during boot; our runtime aborts on the unimplemented import. The caller is sub_827A9290,
which string-compares a launch path against two constants and then tail-calls the import. Reproduced on
8 consecutive headless runs (always 1 VdSwap then abort). One earlier run reached 1380 frames and fired
the hot pair, so the quit is NONDETERMINISTIC, not headless-specific -- a windowed run aborts identically.
This is catalog #16.

DEAD END RULED OUT: 'the draw backend only works windowed' is FALSE. The offscreen render + PPM readback
is the same code with or without a swapchain, and windowed runs hit the identical XamLoaderLaunchTitle
abort. Do not chase a headless-vs-windowed divergence; there is none.

NOT DONE: binding a previously-rendered RT as texture0 instead of the 1x1 stub (milestone step 3).
The backend is frame-agnostic -- GEARS_DRAW_FRAME_AT=N picks which frame to capture -- so it will render
a rich frame unchanged once a run reaches one.

### Note (2026-07-23)
CORRECTION, measured: the title does NOT quit to dashboard during boot on its own. A plain headless run with NO draw capture reaches 2963 frames in 150s and never calls XamLoaderLaunchTitle (0 occurrences). The abort reproduces only with GEARS_DRAW_FRAME=1. So the frame capture/render path is NOT read-only with respect to the guest -- it perturbs the run into the quit path. Leading hypothesis, untested: the frame render is one-shot and SYNCHRONOUS on the command-processor thread, so a long GPU render stalls the CP, the guest's GPU-progress waits time out, and the title takes an error path that terminates. Do not record this as title behaviour or as catalog #16; it is ours.

### Note (2026-07-23)
FALSIFIED, measured 2026-07-23: both the "capture perturbs the guest into XamLoaderLaunchTitle" claim AND the synchronous-CP-stall hypothesis are WRONG. Do not re-chase either.

MEASUREMENTS (all headless GEARS_NO_WINDOW=1, on the SAME commit the earlier note was written at, 7591f67, tree otherwise clean):
- capture ON (GEARS_DRAW_FRAME=1): 14 runs -- 4 x 150s, 2 x 150s with GEARS_DRAW_VALIDATE=1, 8 x 40s. XamLoaderLaunchTitle occurrences: 0 in ALL 14. Frames: 4200/4260/4260/5520 at 150s, 4140/4200 at 150s validated, 840-960 at 40s.
- capture OFF: 10 runs -- 2 x 150s, 8 x 40s. XamLoaderLaunchTitle: 0 in all 10. Frames: 4200/4200 at 150s, 960 uniformly at 40s.
Capture ON and OFF are statistically indistinguishable. The stated 2963 frames/150s capture-OFF baseline does not reproduce either; this machine now does 4200 either way, so that single sample was throughput noise, not a control.

STALL HYPOTHESIS REFUTED DIRECTLY. Instrumented: RenderFrame blocks the CP thread for 163-178 ms (210-235 ms with validation layers) -- device create + 3 shader translations + 3 pipelines + 64 MiB SSBO memcpy + submit + fence + readback, ONE SHOT. That is ~5 frames at 30 fps, not a hang. New diagnostic GEARS_CP_STALL_MS=N blocks the CP thread for N ms at the first swap doing nothing else: at 5000 ms the run continues normally (3300 frames, 0 XamLoaderLaunchTitle) and at 20000 ms it also continues (3360 frames, 0 XamLoaderLaunchTitle, no "WAIT_REG_MEM stuck" report). A stall 120x longer than the real one changes throughput only. So a long CP stall does NOT drive the guest into a terminating error path, and the real 165 ms stall is nowhere near any threshold. Note WaitRegMem in vd_null_gpu.cpp has no timeout at all -- it polls forever -- so there is no CP-side wait that can fail by construction.

The capture output is unchanged and correct: 26 of 26 draws issued, 0 skipped, 2 shader pairs / 3 shaders / 3 pipelines, 6328/921600 px (0.7%), scratch/screenshots/frame.ppm written.

WHAT IS STILL UNEXPLAINED: what caused the earlier 8 consecutive quitting runs. It is not the capture and not a CP stall. Nothing was changed to make it go away -- it simply does not occur now, across 24 runs. The original entry already recorded the quit as NONDETERMINISTIC (one early run reached 1380 frames), so the most defensible reading is a rare nondeterministic guest path that clustered in one session and whose correlation with GEARS_DRAW_FRAME=1 was coincidence, not causation -- the 8-vs-1 comparison was never a controlled experiment. If it recurs, get it under a control arm (alternate ON/OFF runs in one batch) BEFORE hypothesising a mechanism.
