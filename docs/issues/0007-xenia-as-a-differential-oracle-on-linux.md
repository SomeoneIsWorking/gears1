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

### Note (2026-07-22)
Dependency list REFINED by reading Xenia's CMake rather than trusting its Ubuntu apt line. Only three find_package/pkg_check entries exist: PkgConfig, Python3, and GTK3 (the sole REQUIRED one) -- and gtk3-devel is already installed. SDL2 is vendored in third_party, so SDL2-devel is not needed. No libc++ flags anywhere in the CMake, so libcxx-devel/libcxxabi-devel appear unnecessary too.

### Note (2026-07-22)
The one genuine gap is lz4: CMakeLists.txt line 259 does link_libraries(stdc++fs dl lz4 pthread rt), and while liblz4.so.1 is present at runtime, the /usr/lib64/liblz4.so development symlink is missing, so the link step would fail. So the blocker is a SINGLE package (lz4-devel), not the four the apt line implies.

### Note (2026-07-22)
CMake configure SUCCEEDS on this machine (exit 0, Clang 22, CMake 4.3.0, Ninja). It even found system sdl2 2.32.70, so SDL2 was never a blocker. Clone with all submodules is 4.5 GB. Build target for the emulator is xenia-app. Build started; the open question is whether it compiles and whether the only remaining failure is the lz4 link.

### Note (2026-07-22)
CORRECTION: an earlier note that the build 'exited 0' was wrong -- that was the exit status of a trailing echo, not ninja. The build actually FAILED at 489/702. Always check the log, not the wrapper's status, when the command is a pipeline.

### Note (2026-07-22)
Real failure: 'version.h' file not found, included by trace_writer.cc, primitive_processor.cc and main_win.cc. Nothing in the CMake generates it -- Xenia's primary build is premake-based and the CMake path is incomplete, which is what 'experimental Linux support' concretely means here. Not a Clang or Linux incompatibility. Worked around by generating build/generated/version.h from git metadata (XE_BUILD_BRANCH/COMMIT/COMMIT_SHORT/DATE) and adding it to the include path.
