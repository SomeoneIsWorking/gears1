---
id: 128
title: Predicated-tile duplicates converted and uploaded the same index buffer before collapse
status: resolved
symptom: draw-heavy captured frames spend repeated CPU time in index widening and quad expansion even though tiling collapse later merges duplicate submissions
tags: performance,gpu,draw-loop,indices,edram,cache,fixed
created: 2026-08-22
updated: 2026-08-22
---

Root cause: CollapseEdramTiling runs after every draw has been prepared. Predicated EDRAM tiles therefore presented repeated FrameDrawItems to PrepareIndices before the duplicates were merged. Each copy reread the same final-frame guest bytes, byte-swapped and widened them, expanded quad lists, and copied another identical block into the frame arena.\n\nFix: IndexPreparer owns an exact frame-local reuse table keyed by guest base, count, endian, index width, indexed mode, and primitive. A hit reuses the already-uploaded arena buffer/offset and converted count. Non-indexed non-quad draws stay on the zero-buffer path. The table is frame-local, so no guest-memory generation or Vulkan retirement lifetime crosses a frame boundary. The renderer's captured-frame model already exposes one final guest-memory image for the frame; it cannot represent two historical contents at one address within that frame.\n\nEvidence: chapter45_recovered.gfr reported 1276 hits of 1614 reusable draws and 338 exact entries. A temporary interleaved A/B alternated the old conversion path and the cache on byte-identical frames after warm-up: 25.09 ms over 44 old-path frames versus 21.54 ms over 43 cached frames, -3.54 ms against a 0.81 ms resolution threshold. The temporary switch was removed. scratch/logs/index_cache_ab.log is untracked run evidence.\n\nFalsifier: any full-key mutation returning a hit, a reused VkBuffer/offset becoming invalid before the frame fence, a frame model that captures multiple historical index payloads for the same key, or a representative paired replay failing to resolve the cached arm faster.
