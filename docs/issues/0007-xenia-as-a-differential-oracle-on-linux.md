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

### Note (2026-07-22)
Toolchain check on this Fedora machine: Clang 22 (docs want 'Clang-19 or newer' -- satisfied), CMake 4.3.0, Ninja 1.13.2. Build is CMake+Ninja via CMakePresets.json (presets: default, vs, vs-arm64). Clone is ~1.3 GB with submodules and takes well over 10 minutes.

### Note (2026-07-22)
BLOCKED ON SYSTEM PACKAGES. Present: gtk3-devel, vulkan-loader-devel, libX11-devel, libxcb-devel. Missing: libcxx-devel, libcxxabi-devel, lz4-devel, SDL2-devel. Installing these needs sudo, i.e. a machine-level change outside the repo, so it requires the user's go-ahead rather than being done unilaterally.
