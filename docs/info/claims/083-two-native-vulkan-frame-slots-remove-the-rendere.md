---
id: C083
kind: claim
status: holds
created: 2026-08-26
tags: renderer,performance,retirement
depends: runtime/gpu_frame_capacity.h#kSharedScanoutImageCount, runtime/gpu_frame_slots.cpp#GpuFrameSlots::Submit, runtime/gpu_draw.cpp#Renderer::RenderFrameImpl, runtime/render_thread.cpp#RenderThreadMain, runtime/gpu_present_source.cpp#PresentSourceSlots::Begin
---

## Claim

Two native Vulkan frame slots remove the renderer's unconditional producer-fence wait from ordinary live frames while preserving diagnostic readback and guest-retirement ordering.

## Evidence

Catalog #139: validated 25-second headless-present run crossed the heavy frame-570 transition at 29.7-30.0 completed frames/s after warm-up, 5-7 ms CPU preparation/submission, zero renderer-queue drops, and no validation/shared-image/publication errors; synchronous chapter-45 SHA-256 remained 3b34082ab05198fa4733a50c7fe6e671b32c3871be38f7b563154ec741f80c25.

## What would falsify it

A live ordinary frame blocks on its own producer fence before returning, guest EVENT_WRITE_SHD publishes before that fence, a presenter reads an overwritten/unready scan-out image, or the synchronous replay hash changes on the same capture and configuration.
