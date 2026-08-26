---
id: 139
title: Renderer serializes every frame on one Vulkan fence
status: resolved
symptom: Current chapter-45 replay spends about 14 ms per frame in submit+wait and live rendering cannot overlap CPU command recording with GPU execution
tags: performance,renderer,vulkan,retirement
created: 2026-08-26
updated: 2026-08-26
---

Root cause: the renderer owns one mutable command/fence/descriptor/arena/SSBO/readback resource set and waits its fence unconditionally before returning. The presenter also consumed naked scan-out images, so simply removing the wait would race producer reuse against presenter reads.

Proper fix: bounded per-frame Vulkan resources with autonomous ordered completion, generation-specific guest retirement, and producer/presenter scan-out leases. Diagnostic report/probe paths stay explicitly synchronous.

### Resolution (2026-08-26)
Integrated two per-frame Vulkan resource slots, autonomous submission-ordered fence completion, deferred transient cleanup, five scan-out images derived from the two renderer slots plus two presenter slots plus one retained latest publication, and presenter-fence source leases. Live validated headless-present evidence: 25 seconds through the frame-570 heavy transition, 29.7-30.0 completed frames/s after warm-up, 5-7 ms/frame CPU preparation/submission, zero renderer-queue drops, and no Vulkan validation/shared-image/publication errors. Synchronous chapter-45 output remains SHA-256 3b34082ab05198fa4733a50c7fe6e671b32c3871be38f7b563154ec741f80c25. The separate title-side ~30 Hz cap and unmeasured sub-16.67 ms GPU budget remain renderer-60hz work, not part of this root cause.
