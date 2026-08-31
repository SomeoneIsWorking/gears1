---
id: 157
title: Native pass translated before substitution blocked performance
status: resolved
symptom: GEARS_NATIVE_PASSES=1 still translated the implemented scene-composite stages before selecting their native modules
tags: performance,native-rhi,native-pass,render,shader
created: 2026-08-28
updated: 2026-08-29
state_items: S004,S005,S007
---

## Root cause

`ShaderCache::GetShader` always called `TranslateShader` on a cache miss and
only selected `native_pass::Find` afterward. The native module therefore
changed the Vulkan shader body but did not remove the Xenos analysis-to-SPIR-V
translation cost. The rest of the draw setup also depended on metadata carried
by the translated `ShaderXlate`.

## Resolution

The shared `ShaderInterface` now owns the metadata consumed by descriptor,
uniform, sampler, and vertex-fetch setup. The scene-composite pass provides its
observed contract directly, and the native cache-miss path copies that contract
without invoking Xenos translation. The pass validates its required pixel
interpolator mask against the draw modification and refuses an incomplete or
mismatched contract. Translation remains available when native passes are off
or when `GEARS_NATIVE_PASSES_KEEP_TRANSLATED=1` is explicitly requested.

The same `title600.gfr` capture was replayed headlessly on both arms. The
native log records direct-interface messages and no translated scene-composite
vertex or pixel shader; the retained inspection arm records translation and
selects the same native modules. The vertex module is grounded by the observed
fetch-95 layout (12 dwords per vertex, position at offset 0, UV at offset 4,
colour/interpolator data at offset 6). Three sequential four-repeat sweeps measured
5.574/7.305/7.411 ms GPU retained versus 5.276/5.709/7.157 ms native. The
native values are directionally lower on average, but the spread is too noisy
for a stable speedup claim. `tools/compare_frames.py` reported a match within
0.001 mean channel difference, worst channel 4/255, and `GEARS_DRAW_VALIDATE=1`
exited without image-interface diagnostics. The complete native RHI frontend
and the 8.33 ms/120 fps target remain unresolved.

## Evidence

- `scratch/logs/native-bypass-compat-dir.log`
- `scratch/logs/native-bypass-native-dir.log`
- `scratch/logs/native-bypass-final-compat.log`
- `scratch/logs/native-bypass-final-native.log`
- `scratch/logs/native-bypass-final-validate.log`
- `scratch/logs/native-bypass-sweep-c1.log` through `scratch/logs/native-bypass-sweep-n3.log`
- `scratch/logs/native-bypass-inspection.log`
- `scratch/logs/native-bypass-validate.log`
- `scratch/native-bypass-compat/frame_00602.ppm`
- `scratch/native-bypass-native/frame_00602.ppm`
- `scratch/logs/native-scene-composite/xlate.log`
- `scratch/logs/native-scene-composite/native.log`
- `scratch/logs/native-scene-composite/validate.log`
- `scratch/screenshots/native-scene-composite-xlate/frame_00602.ppm`
- `scratch/screenshots/native-scene-composite-native/frame_00602.ppm`
