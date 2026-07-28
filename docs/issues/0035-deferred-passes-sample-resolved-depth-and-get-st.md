---
id: 35
title: Deferred passes sample resolved DEPTH and get stale guest memory
status: open
symptom: 30 texture bindings in an Act 1 frame name a depth resolve destination; we do not serve depth resolves, so those bindings decode whatever the guest left in memory
tags: gpu,draw,draw-backend,resolve,depth,deferred,gameplay
created: 2026-07-28
updated: 2026-07-28
---

MEASURED, and it CORRECTS a claim I put on the re-frontier last iteration.

I recorded "only 24 of 5278 texture bindings are served by a resolve target" as
the largest structural gap, implying most bindings were being mis-served. The
census now classifies every base three ways -- IS a resolve target, falls INSIDE
one, or is an ordinary guest asset -- and the ratio is LEGITIMATE:

    136 distinct texture bases, 5278 bindings
     24 bindings name a colour resolve target, and are served from it
      0 bindings fall inside a resolve target (no sub-rectangle routing miss)
   ~5224 bindings are ordinary guest assets -- world art, correctly uploaded

So the frame is not mostly mis-served. Most texture bindings genuinely are guest
textures, and 24 render-target samples in a frame is a normal number.

THE REAL GAP is narrower and specific. Thirty bindings name a DEPTH resolve
destination:

    0xba40000   28 bindings   (tile 1's depth resolve destination)
    0xc510000    2 bindings

We do not serve depth resolves -- there is no host depth texture chain -- so
every one of those 30 bindings falls through to uploadTexture and decodes
whatever the guest last left at that address. On a deferred renderer these are
almost certainly the lighting and post passes reconstructing position from depth,
which is why it matters out of proportion to the count.

WHAT IT NEEDS, and why it is not just "run the existing resolve on the depth
image": the guest resolves depth to a k_8_8_8_8 destination.

    draw 408: depth@0x0 -> 0xba40000  pitch 1280 height 720
              format 6 (k_8_8_8_8) number 2 endian 2 (k8in32) exp_bias 0

That is the Xenos depth-as-colour resolve: the 24_8 depth/stencil is copied into
an 8888 texture and the sampling shaders decode it. Our host depth is
D32_SFLOAT, so serving this needs an ENCODE -- float32 depth back to the guest's
24-bit depth format (float24 20e4 for kD24FS8, unorm24 for kD24S8) packed with
its stencil into 8888, with the k8in32 endian applied -- not a copy.

The compute resolve built for catalog #33 is the right vehicle: this is a second
shader variant reading the depth image and writing the packed 8888 texture. The
20e4 encode is the inverse of the Depth20e4To32 already ported for the depth
clear.

### Note (2026-07-28)
HALF IMPLEMENTED. The encode and the shader exist and build; the wiring into the
renderer does not, so nothing has changed behaviourally yet (the replayed frame
is byte-identical, 0 of 2764816).

What is done, and verified:

  The float32 -> float24 (20e4) ENCODE, ported from Xenia's
  PreClampedDepthTo20e4 (itself CFloat24 from d3dref9.dll): denormal path,
  normal path, and round-to-nearest-even. Verified as the exact inverse of the
  Depth20e4To32 already in the tree -- 200001 round trips sampled across [0,1],
  ALL within one ULP of the 20-bit mantissa, and the anchors exact: 0 -> 0x000000,
  1 -> 0xf00000, 0.5 -> 0xe00000, 0.25 -> 0xd00000.

  BuildDepthResolveComputeShader. Separate from the colour resolve shader for two
  reasons that are not stylistic: a depth image CANNOT be a storage image on
  Vulkan, so the source is bound as a SAMPLED image and read with OpImageFetch
  (no sampler needed -- VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE gives an OpTypeImage
  with Sampled=1); and the destination is not a copy of the source but an
  encoding of it. The shader encodes to float24 or unorm24 per the depth format,
  packs depth into bits 8..31 with stencil in 0..7 exactly as RB_DEPTH_CLEAR is
  laid out, and writes the four bytes as normalised components -- which is what
  a fetch of the guest's k_8_8_8_8 destination would hand the shader.

STILL TO DO: the pipeline (a different descriptor layout from the colour
resolve), a sampled view of the host depth image, resolve targets for depth
destinations (they are currently dropped in the pre-pass), and the dispatch.

ONE ASSUMPTION IS NOT YET DERIVED FROM ANYTHING, and is flagged in the shader
itself: the BYTE ORDER of the packed dword across the four components. It is
written most-significant-first. Everything else here comes from a register or
from Xenia; this does not, and the way to settle it is to disassemble the pixel
shader that samples 0xba40000 and read how it recombines the components -- the
microcode is in the frame capture. A wrong order will show as a depth image that
bands or looks like noise rather than a smooth ramp, so the resolve-target dump
can also falsify it.

STENCIL is written as zero. We carry no stencil buffer; recorded rather than
hidden, because a title that tests resolved stencil would need it.
