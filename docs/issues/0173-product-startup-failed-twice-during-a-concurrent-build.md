---
id: 173
title: Product startup failed twice during a concurrent heavy build
status: investigating
symptom: two offscreen runs never presented, one hung with every guest thread idle after thread 13 started, one aborted with "double free or corruption (!prev)" just after Xenia loaded stored pipeline descriptions
tags: stability,xenia,shader-storage,startup
state_items: S009
created: 2026-09-22
updated: 2026-09-22
---

## Finding

Both failures happened while a 16-job Xenia build was running on the same
host. The hang stopped at 6,257 translated functions from the first second
on. The abort followed `Loaded 248 pipeline descriptions, 269 shader
translations needed`, which places it in the pipeline cache's
translate-on-load callback or in whatever runs right after it.

## What was tried / dead ends

- The same storage root and disc ran cleanly 4 of 4 times on an idle host,
  and 4 of 4 times with all 16 cores saturated by busy loops. CPU load
  alone does not reproduce it.
- A run under gdb (Xenia's SIGSEGV/SIGILL handling passed through) did not
  fail.
- Shared-memory naming is not the cause: Xenia names its guest mapping from
  the host tick count and unlinks it after opening, and `/dev/shm` held no
  leftover mappings.

## Required work

Reproduce it again, keeping the core dump, and name the cause before
changing anything. Memory pressure from the concurrent build is the
untested candidate. Until then, a clean run is not evidence that startup is
race-free.
