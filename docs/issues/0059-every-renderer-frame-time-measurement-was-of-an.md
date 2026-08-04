---
id: 59
title: Every renderer frame-time measurement was of an unoptimised build
status: resolved
symptom: The texture staleness hash reads guest memory at 0.95 GB/s; the same hash over the same kind of mapping runs at 7-22 GB/s in a standalone -O2 benchmark
tags: performance,build,instrument,gpu,draw
created: 2026-08-05
updated: 2026-08-05
---

## Symptom

The renderer's per-frame texture staleness check reported 15.55 MiB hashed in
16.1 ms -- 0.95 GB/s. A standalone benchmark hashing 15 MiB with the same XXH3,
out of a memfd mapped exactly the way guest RAM is (MAP_SHARED, second alias of
the same descriptor), ran at 7-22 GB/s. Same bytes, same hash, ten times the
speed.

## Cause

`CMAKE_BUILD_TYPE=Debug`, and `CMAKE_CXX_FLAGS_DEBUG` is `-g`. The 191 generated
guest translation units carry their own `-O2` through `GEARS_PPC_OPT`, so the
guest side was always optimised and nobody noticed that **every host source was
at -O0** -- the renderer, the texture decode, the shader translation, the
descriptor assembly, all of it.

Every frame-cost number this project has recorded was of a debug renderer.

## Fix

`GEARS_HOST_OPT` (default `-O2`), applied to gears1, gears_draw, gears_draw_xlate
and frame_replay, alongside `-g`. The build type stays Debug for the guest side.
Set `GEARS_HOST_OPT=-O0` to bisect a miscompile in our own code, the way
`GEARS_PPC_OPT=-O0` does for the guest's.

## Measured, same captured Act 1 frame, warm, byte-identical output (sha256 1cb8934c)

|                          | -O0    | -O2   |
|--------------------------|--------|-------|
| staleness hash (15.55 MiB)| 16.1 ms| 1.2 ms|
| draw loop                | 37-44 ms| 16 ms|
| whole frame              | 45-74 ms| 29 ms|

Live in gameplay the renderer went from ~12 fps at 67 ms to ~14 fps at 53 ms; the
rest of the gap to the offline 29 ms is contention with the guest's own threads on
a machine that was also running other builds.

## The lesson

The profile was honest about WHERE the time went and silent about WHAT it was
measuring. A per-phase breakdown that adds up perfectly can still be describing a
build nobody intends to ship, and the tell was a rate -- bytes per second -- not a
proportion. Rates can be checked against physics; percentages cannot.
