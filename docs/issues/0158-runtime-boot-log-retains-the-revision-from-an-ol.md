---
id: 158
title: Runtime boot log retains the revision from an older CMake configure
status: resolved
symptom: After a newer commit, ./run.sh builds with ninja reporting no work and gears1 logs an older Git revision
tags: build,launcher,diagnostic,cmake
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

`runtime/CMakeLists.txt` resolved Git `HEAD` only while CMake configured and
embedded that value as a `main.cpp` compile definition. No build dependency
represented later commits, so the build graph correctly considered `main.cpp`
current while its diagnostic identity was stale.

## Resolution

The `gears_build_revision` target now invokes the locked
`tools/write_build_revision.py` publisher before every `gears1` target check.
It rewrites a generated header only when `HEAD` changes, making the header a
real compiler dependency without recompiling an unchanged commit.

## Evidence

- `build_revision_selftest` creates two real commits and proves the header
  changes between them while an unchanged revision preserves its timestamp.
- The no-change Ninja check ran the publisher without rebuilding `main.cpp`.
- After commit `1bb843b`, `./run.sh --headless --http-port 0` updated the
  generated header from `7de592e` to `1bb843b`, rebuilt `main.cpp`, linked the
  runtime, and logged `gears1 built Aug 30 2026 05:30:53 from 1bb843b`.
- `scratch/logs/build-revision-1bb843b.log`
