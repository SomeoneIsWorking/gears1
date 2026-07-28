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

### Note (2026-07-28)
THE BYTE-ORDER QUESTION IS SETTLED, AND THE ANSWER RETIRES IT. Reading the
shader, as planned, changed the design rather than confirming it.

Two measurements:

  The frame binds the depth resolve destination as a DEPTH texture, not a colour
  one --

      texture 0xba40000 k_24_8_FLOAT 1280x720x1 dim1 tiled endian2 mips0-0 x2

  and it is one of the three fetches reported as "no host format mapping". The
  frame has exactly two k_24_8_FLOAT fetches and exactly two depth resolve
  destinations.

  The shaders that sample it take only .x. ps_db24986d2cc37fb0 does
  `tfetch2D r4.x___, r4.wx, tf3` and `tfetch2D r5.x___, r5.xy, tf3` -- one
  component, twice.

So the guest WRITES the resolve as k_8_8_8_8 bytes and READS the same bytes as
k_24_8_FLOAT: the texture unit decodes the packed depth and hands the shader a
float. Nothing ever observes those bytes as colour.

Since we bypass guest memory entirely, packing the bytes only to have them
unpacked again is a round trip through a representation nobody observes. The
depth resolve shader now writes what the FETCH would have produced -- depth in
.x, stencil in .y -- and the byte-order assumption I flagged last iteration is
gone rather than resolved: there are no bytes.

A CENSUS BUG had to be fixed to get here, and it is worth recording because it
nearly sent the investigation somewhere useless. The census recorded the pixel
shader hash for each texture binding, but set it AFTER selectTexView had already
run for that draw -- selectTexView is called while the descriptor sets are built,
which is earlier than the PreparedDraw is filled. Every binding was attributed to
the PREVIOUS draw's shader. The first shader it named, ps3f8dacf87fb8da17, is 15
dwords with no texture fetch in it at all, which is the only reason the
misattribution was caught rather than acted on.

STILL TO DO: the pipeline (a sampled-image descriptor layout, unlike the colour
resolve's two storage images), a sampled view of the host depth image, resolve
targets for depth destinations (currently dropped in the pre-pass), and the
dispatch.

NOT applied, and named: the guest's float24 quantisation. Its depth carries 20
mantissa bits against our 32, so a sampled depth is up to one 20-bit ULP finer
than the console's. Depth32To20e4 stays in the tree, verified as the exact
inverse of the decode, for if that ever needs to be exact.
