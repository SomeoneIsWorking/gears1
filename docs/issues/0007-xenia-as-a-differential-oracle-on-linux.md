---
id: 7
title: Xenia as a differential oracle on Linux
status: investigating
symptom: need a reference emulator to compare guest state against; unclear whether Xenia builds and runs on Linux
tags: harness,method,oracle
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
In favour: Xenia ships per-instruction PPC tracing (ITRACE/DTRACE in x64_tracers.cc, --store_all_context_values, TARGET_THREAD), which is exactly what a differential harness needs and is the largest piece of the work already done.

### Note (2026-07-22)
Against: upstream docs/building.md says verbatim 'Linux support is extremely experimental and presently incomplete', and expects Clang 19 specifically (this machine has Clang 22).

### Note (2026-07-22)
Bounded feasibility test started rather than committing to the full harness: clone and attempt a Linux build. A failed build is still a useful result -- it resolves the harness-vs-broaden fork empirically instead of leaving it open. Recursive clone exceeds 10 minutes, so it runs in the background; artifacts under scratch/oracle/ (gitignored).
