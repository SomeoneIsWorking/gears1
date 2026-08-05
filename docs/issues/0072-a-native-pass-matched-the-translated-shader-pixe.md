---
id: 72
title: A native pass matched the translated shader pixel for pixel and still had the wrong descriptor view type
status: resolved
symptom: a native pass is bit-exact against the translated shader, and Vulkan validation still reports 'VkImageViewType is VK_IMAGE_VIEW_TYPE_2D_ARRAY but the OpTypeImage has (Dim = 2D) and (Arrayed = 0)'; or a native pass is off by one on a handful of dark pixels with no arithmetic error to find
tags: native-renderer,gpu,shaders,spirv,descriptors,validation
created: 2026-08-05
updated: 2026-08-05
---

## Symptom

Two things that turned out to be the same root cause.

1. `uber_post_blend.frag` (pixel shader `0x9610bf8038af9aaf`) shipped with **20
   of 2,764,800 channel samples off by one** on the courtyard capture. Every one
   was a dark pixel (values 2-5) where the output gamma's `trunc` sits next to a
   boundary, 17 of 20 one level BRIGHTER than the translated pass. The decoded
   arithmetic was correct.
2. `movie_yuv.frag` and `scene_gamma.frag` were **bit-exact** and had been for
   two sessions, and were still wrong.

## Root cause: the FETCH interface, in three places

A native pass has to implement the translator's interface exactly, and none of
these is visible in the microcode -- only in the module the runtime builds.

- **Texture size.** The translator reads width and height from the fetch
  constant's dword 2 (two 13-bit fields holding `size - 1`) and scales the
  0.75/512 texel rounding offset by THAT. All three shaders used
  `textureSize()`, the host image's extent.
- **Gradients.** The translator emits `OpDPdxCoarse`/`OpDPdyCoarse`, scales them
  by `exp2(lodBias / 32)` from fetch-constant dword 4 bits 12..21, and calls
  `OpImageSampleExplicitLod ... Grad`. All three used implicit-LOD `texture()`,
  which lets the driver choose its own derivative precision. **This is what
  produced the 20 samples.**
- **Dimensionality.** The translator declares `OpTypeImage %float 2D 0 1 0 1` --
  Arrayed = 1 -- and `gpu_draw.cpp` binds guest textures as
  `VK_IMAGE_VIEW_TYPE_2D_ARRAY` (two sites). All three declared `texture2D`.

Fetch constant `k`'s dword `d` lives at `[(6k+d)/4][(6k+d)%4]` of the uvec4[48]
at set 1, binding 4. Verified against the dumped module: size at [0][2] for tf0,
[2][0] for tf1, [3][2] for tf2.

## Why the gate could not find it

**A pixel-comparison gate audits a RESULT, not an interface.** The driver
tolerates a 2D view declaration against a 2D_ARRAY view and samples layer 0
anyway, so `movie_yuv.frag` and `scene_gamma.frag` produced identical pixels
while being wrong. `tools/verify_native_pass.sh` would never have reported it.

`GEARS_DRAW_VALIDATE=1` reports it in one line per draw:
`the sampled image descriptor [Set 3, Binding 0, "SceneColor"] VkImageViewType
is VK_IMAGE_VIEW_TYPE_2D_ARRAY but the OpTypeImage has (Dim = 2D) and
(Arrayed = 0)`.

**Run `GEARS_DRAW_VALIDATE=1` on every native pass.** The A/B gate is necessary
and not sufficient.

## Resolution

All three shaders now read the size and LOD bias from the fetch constants, sample
with `textureGrad` on coarse derivatives, and declare `texture2DArray`. Re-verified:

- `uber_post_blend`: 2,764,800 / 2,764,800 on courtyard and on act1_v2
- `scene_gamma`: 2,764,800 / 2,764,800 on act1_v2 and on bright
- `movie_yuv`: 2,764,800 / 2,764,800 on boot150

each with a negative control that reported a difference in the same run, and zero
view-type warnings under validation.

## Dead end, recorded

The first suspect for the 20 samples was compiler contraction of instruction 29's
`mad` into an FMA. Splitting it under `precise` changed **nothing** -- the count
stayed at exactly 20. The split is still in the shader because it is what the
microcode does, commented as unmeasured so it is not cited as the fix.

## Also worth not re-deriving

Texture bindings are numbered by **order of first use in the shader**, not by
fetch constant. `0x9610bf8038af9aaf` fetches tf2 first, so set 3 binding 0/1 is
texture**2**, 2/3 is texture0, 4/5 is texture1, and the samplers are 6, 7, 8 in
the same order. The movie pass's `0 = texture0` layout is a coincidence of it
fetching tf0 first.

New knob: `GEARS_DRAW_SPV_DUMP=<dir>` writes the translated module as the runtime
built it for a draw's modification key. The offline modules in
`scratch/shaders/bound_out/` are translated with NO modification, so they have no
interpolator inputs and a colour write mask of zero.
