---
id: 77
title: Our Act 1 frame against the Xenia oracle: five differences
status: open
symptom: our in-game frame is missing the character and HUD, windows are flat grey blocks, vertical streaking, lifted blacks
tags: render,oracle,gameplay-scene
created: 2026-08-05
updated: 2026-08-06
---

The first oracle-backed comparison of an in-game frame, both sides headless and
driven from the SAME scripted walk (`tools/oracle_compare.sh`): our renderer via
its own filmstrip, Xenia via `tools/xenia_oracle`.

WHAT KIND OF COMPARISON THIS IS. Two separate emulations at matching wall-clock
offsets, NOT frame-synchronised, so a pixel metric between them is meaningless
and none is quoted. Both frames show the same Act 1 wall from very nearly the
same camera -- same three windows, same moss line, same wall panel -- which is
what makes the differences below readable.

  ours:   scratch/oracle/compare/ours/frame_06300.ppm  (~210 s in)
  theirs: scratch/oracle/compare/theirs/frame_0210s.png

FIVE DIFFERENCES, most severe first:

1. NO CHARACTER. Marcus is absent from our frame. This is a third-person game;
   the player character is in frame during all gameplay, so this is not a camera
   difference.
2. NO HUD. The oracle draws the ammo counter and weapon icon; ours has neither.
   That 1 and 2 are BOTH missing is the interesting part -- both are late passes,
   which points at our frame ending before them or dropping them, rather than at
   two unrelated faults.
3. WINDOWS ARE FLAT GREY BLOCKS. The oracle shows sky, bars and light shafts
   through each window; ours fills them with uniform light grey.
4. VERTICAL STREAKING across the image, strongest on the right.
5. LIFTED BLACKS / LOW CONTRAST, and this one is measured rather than eyeballed:
   ours mean 30.3, 6,711 distinct colours, 95.1% of pixels above 8/255; theirs
   mean 22.1, 24,497 colours, 75.8%. Ours is brighter with a THIRD of the colour
   variety, which is the signature of a flattened tonemap rather than of a
   different moment.

NOT YET DIAGNOSED. Recorded so the next session starts from the observation
rather than from a fresh playthrough. The obvious first question is whether 1
and 2 share a cause with the project's existing note that the presented surface
is chosen by rule when the guest's front buffer names no resolve destination.

### Note (2026-08-05)
## Confirmed in-game, so this is not a "different moment" (2026-08-05)

The first reading to rule out was that our run simply had not reached gameplay --
a menu or attract camera would explain a missing character and HUD without any
renderer fault. It has:

  [draw] frame: 639 of 849 draws issued, 0 skipped; 107 distinct shader pairs,
         275 distinct shaders, 254 pipelines; 990 texture bindings (934 guest
         textures, 56 from the rendered RT); 886987/921600 px non-black (96.2%)

849 draws with 107 shader pairs is the Act 1 deferred pipeline, not a menu (a
menu frame in the same run is 177 draws / 23 pairs). Our frames alternate
~690-720 and ~235-265 draws issued, which is the predicated tiling pattern.

Also ruled out: the PRESENTATION CHOICE. The log says "presenting surface 0x2d0,
chosen by the guest's front-buffer address (front buffer 0xa0311000)" -- so we
are presenting the surface the guest itself named, not a rule-of-thumb fallback,
and the missing passes are missing from the buffer the guest points at.

The 210 draws not issued are mostly accounted for by the tiling collapse ("184
replayed draws and 2 resolves dropped"), which is by design; ~24 are not
accounted for by that line and nobody has looked at what they are. That is the
first thing to check next, because "the character's draws" is exactly the shape
of a couple of dozen draws that quietly do not reach the backend.

### Note (2026-08-05)
## CORRECTION: there are no unaccounted draws (2026-08-05)

The note above ended by pointing at "~24 draws the tiling-collapse accounting
does not explain ... the right size for a character pass". That was arithmetic
done across two different frames' log lines, and it is WRONG.

Checked properly on one frame, replaying courtyard.gfr offline:

    552 issued + 174 dropped by the tiling collapse + 18 resolve draws = 744

which is exactly the frame's draw count. Every draw is accounted for. Nothing is
quietly failing to reach the backend, and the next session should not go looking
for it.

So the character and HUD are missing for one of two reasons, and the arithmetic
cannot tell them apart:
  (a) the guest never submitted those draws in this frame, or
  (b) they were issued and rendered nothing visible -- wrong transform, culled,
      degenerate, or written into a surface we do not present.

The offline loop is what makes this cheap to pursue: `frame_replay` on
scratch/frames/courtyard.gfr reproduces the symptom in about fifteen seconds
(same wall, no character, no HUD, two of three windows flat grey, and the image
pillarboxed inside a black border), so no further playthroughs are needed.

### Note (2026-08-06)
## The character is not being DRAWN, and the camera is under control (2026-08-06)

Two measurements narrow findings 1 and 2 (no character, no HUD) considerably,
and they move the suspicion off the renderer.

FIRST, we are in real gameplay under player control. Driving the left stick
through GEARS_INPUT_SCRIPT moves the view: across checkpoint frames 85-100% of
pixels change, and the run ends looking at a different corner of the room from
where it started. This is not a fixed intro camera.

SECOND, the frame contains no constant-skinned geometry. Across every distinct
vertex shader in two separate captures -- courtyard.gfr and a new walk_v3.gfr
taken WHILE the stick was driving the camera -- the largest float-constant count
is 25:

    counts: 0 x2, 1 x5, 4 x4, 8 x3, 12 x1, 13 x2, 14 x4, 15 x1, 16 x3,
            22 x2, 25 x1

A UE3 skinned character needs a bone palette -- three float4 per bone, dozens of
bones, so 60-200+ constants. Nothing here is close.

WHAT THAT ESTABLISHES, and what it does not. It rules out the reading that the
character's draws are issued and then lost somewhere in our backend: on this
evidence there are no such draws to lose. It does NOT prove the guest never
draws a pawn, because a skinned mesh whose bones arrive some other way -- a
texture fetch, a vertex fetch from a skinning buffer, or CPU skinning -- would
not show up in a constant count. That distinction is unresolved.

Also ruled out earlier and worth keeping together: every draw fetches inside the
SSBO mirror ("726 draws fetch vertices inside the 0x20000000-byte mirror, 0
fetch PAST it"), so this is not the vertex data being unreachable.

So the next question is a GUEST-side one -- whether the title creates and
submits a player pawn at all -- rather than a backend one. Filed here rather
than acted on, because it is a different area of the project from the render
comparison this entry started as.

### Note (2026-08-06)
## A better metric for finding 5, and it points at dynamic range (2026-08-06)

Counting distinct COLOURS was the wrong instrument: a colour triple count
depends on what is in the scene, and the two sides are at different moments, so
"5,807 against 24,000" could never separate a rendering difference from a
content one. Counting distinct LEVELS PER CHANNEL does not have that problem --
any lit surface produces a gradient, whatever it is a picture of.

    ours, gamma ramp applied     R  53  G  61  B  61   of 256   mean 17.4
    ours, no ramp                R  69  G  77  B  77            mean 30.3
    oracle                       R 238  G 238  B 238            mean 22.1
    oracle, a darker frame       R 238  G 238  B 238            mean 20.9

The reference uses nearly the whole 8-bit range on both of its frames. We use
about a quarter of it. That is a compressed DYNAMIC RANGE, not a different
picture, and it is the same defect finding 3 describes: our windows are flat
grey blocks where the reference shows sky, bars and light shafts through them.

Where it is NOT: the scene buffer. At draw 480 the linear-light target carries
184,909 distinct colours and the windows are saturated pure white, so the range
is already gone BEFORE the composite -- the window pixels are clipped in the
lit scene itself, and the composite then maps a range that no longer has
anything at the top of it.

So the next question is why the scene pass saturates where the reference does
not: exposure applied twice, a missing sky/backdrop pass behind the windows, or
the wrong scale on the light accumulation. That is a narrower question than
"our colours are wrong", and the per-channel level count is the metric to judge
a fix by -- it moves when the range is restored and does not move when the
scene merely changes.

Note the gamma ramp REDUCED the level count (77 -> 61), which is expected and
not a regression: the ramp compresses the low end, so 8-bit input mapped through
it lands on fewer distinct outputs. Applying it in higher precision before the
final quantisation would keep those levels; that is a refinement, not the cause
of the gap against the reference.

### Note (2026-08-06)
### Correction to the note above, same day

I wrote that applying the gamma ramp "in higher precision before the final
quantisation would keep those levels; that is a refinement". That is wrong about
the hardware.

The front buffer this title presents is k_8_8_8_8 (measured, from the fetch
constant the guest hands VdSwap). Scan-out therefore indexes the 256-entry LUT
with an EIGHT-BIT value -- the same quantisation our implementation performs --
and emits ten bits to the display. So there is no higher precision to apply the
ramp at on the input side; the only thing we lose that the console has is the
10-bit output, and that is lost in writing an 8-bit PNG, not in the ramp.

Our ramp application is faithful. The reduced level count after applying it is
what the console's own pipeline produces at 8-bit output precision, and is not
part of the dynamic-range gap this entry is about.

### Note (2026-08-06)
## Following the window pixel through the frame, and two probe traps (2026-08-06)

Traced pixel (400,255) -- inside a window -- and (640,350) -- bare wall -- with
GEARS_DRAW_PIXEL_TRACE on the replayed courtyard frame.

    surface 0x2d0, window pixel        surface 0x2d0, wall pixel
    draw 612  (2.66, 2.97, 3.03)       draw 612  (0.53, 0.63, 0.80)
    draw 643  (1, 1, 1)                (not touched)
    issued 527 (0.297,0.297,0.269)     issued 527 (0.205,0.218,0.208)
    issued 528 (0.269,0.297,0.297)     issued 528 (0.208,0.218,0.205)

and the final image agrees exactly: window (68,76,76), wall (53,55,52).

So the HDR data is RIGHT where it is produced -- a 5x brightness ratio between
window and wall -- and the presented image has a 1.4x ratio. The compression
happens on surface 0x2d0, between the copy at draw 612 and the final content.

## Two things the probes made me get wrong first

TRAP 1: the probes sample WHICHEVER surface is bound. A frame switches between
several, so rows interleave buffers, a target switch reads as a value change,
and a value can look like it came from a draw that never touched it. Fixed:
GEARS_DRAW_SURFACE=<hex> restricts both probes to one EDRAM surface, and the
trace above uses it.

TRAP 2: draw 643 writing (1,1,1) over the HDR value looked like a clamp. It is
not. Disassembling its pixel shader (0x3f8dacf87fb8da17):

    mad_sat r0.x___, r1.zzzz, c0.xxxx, c1.xxxx
    maxs_sat oDepth.x___, r0.xx
    max oC0, r0.xxxx, r0.xxxx

with c0.x = 0.0028078845 and c1.x = 0.017 read from that draw's own register
file: saturate(distance/356 + 0.017), i.e. a LINEAR DISTANCE ENCODE. The window
looks at distant sky, so saturating to 1.0 is correct, and surface 0x2d0 is
serving as a depth-encode buffer at that moment, not as colour. Reading it as
colour is what made it look like a defect.

## What is left, and what it is not

The final content appears at issued draw 527, attributed to original draw 702 --
which is prim=1 with ONE index, a single point, and cannot paint two pixels 500
apart. Draw 703 then writes the same value with RED AND BLUE SWAPPED
(0.297,0.297,0.269 -> 0.269,0.297,0.297). So neither of those draws is painting
the scene: the surface's content is being changed by something that is not a
draw -- a resolve reload or a FORMAT REINTERPRETATION.

That fits what the render-target cache already reports for this surface:

    new surface 0x2d0 -> k_8_8_8_8 k_2_10_10_10 k_2_10_10_10_FLOAT k_16_16
    k_2_10_10_10_FLOAT_AS_16_16_16_16 in one host target 1280x720
    (reinterpreted mid-frame; widened host format)

One host target serving five guest formats is already listed as an untested
candidate on the colour step of the RE frontier. This is the first evidence
pointing AT it: the guest writes HDR through one interpretation and reads it
through another, and our single widened float target does not reproduce the bit
reinterpretation. Next session should start there, with the R/B swap between
draws 702 and 703 as a second clue that a format's channel order is in play.

### Note (2026-08-06)
## A sixth difference, and this one is measured: the COLOUR CAST (2026-08-06)

The five differences here are the eye's; the entry says a pixel metric between
the two sides is meaningless. That is true of a PIXEL metric. It is not true of
a CHROMATICITY DISTRIBUTION, which is exposure-invariant and moves slowly as a
camera walks one environment -- and `tools/chroma_compare.py` (new) measures the
null band for it, so the claim is calibrated rather than asserted.

    5 of our gameplay frames x 4 of theirs, same oracle_compare.sh run
    same-renderer null band                 up to 0.0138
    cross-side under an R/B exchange        0.008 .. 0.018   (20 of 20 pairs)
    cross-side as we present it             0.064 .. 0.086

Add to the list, as #6: OUR FRAME IS COOL WHERE THEIRS IS NEUTRAL-WARM, by
almost exactly an R/B exchange. It is not visible next to differences 1-5 by eye
because the frame is desaturated, which is why it went unlisted for a day.

Difference 5 (lifted blacks / low contrast) and this one are independent: the
measurement is invariant to exposure by construction, and its selftest proves it
(a 0.30x copy must still report identity).

The mechanism, and the contradiction it runs into, are on catalog #62 -- the
guest's own scanout swizzle says our present path is RIGHT, so this is not the
front-buffer-vs-source choice. Do not chase it here.

Also confirmed by eye against `scratch/oq/theirs_play.png` and `ours_play.png`
(downscaled side by side): differences 1, 2, 3 and 4 all reproduce exactly as
described, on the current build, from the same two frames. Nothing here has
gone stale.

### Note (2026-08-06)
## The missing character is NOT a clipping fault -- measured, not assumed (2026-08-06)

Difference 1 (no character) and 2 (no HUD) are still open, but the biggest-
looking lead is now closed off, which matters because it would have absorbed a
session.

`walk_gameplay.gfr` kills **276 of its draws at clip or cull** -- and crossing
the per-draw verdict with `tools/pass_structure.py`'s attribution, that is
**120 of 174 BASEPASS draws (69%)** and 115 of 167 PREPASS draws (the same 69%,
as expected since they draw the same objects). A renderer losing 69% of its
world geometry is exactly what a missing character looks like.

It is legitimate. `tools/clip_check.py` (new, the general form of
`clip_volume_check.py`) pushes each draw's dumped vertices through its own world
matrix and view-projection, on this capture:

    draw 292  shaded                  4 of 4 vertices INSIDE   ndc x +0.96..+1.00
    draw 293  killed_by_clip_or_cull  0 of 4 -- w = -499 .. -241,  BEHIND CAMERA
    draw 294  killed_by_clip_or_cull  0 of 4 -- w = -3108 .. -2619, BEHIND CAMERA

293 and 294 are two instances of one mesh (shared vertex buffer 0xe840000, world
translations (2066,-536,2491) and (-930,-3019,2223)) and the guest itself places
both behind the camera. The run is CALIBRATED: draw 292 is one the GPU
rasterised, and the same arithmetic puts it inside, so the layout is checked
against a case whose answer is known rather than only against the ones it
expects to reject.

That is the same shape as catalog #74's windows, where the clip was also
exonerated. Two independent cases now say our clip agrees with the guest.

## So what IS left for differences 1 and 2

Not the clip, and not culling (#74 already ruled that out with
`GEARS_DRAW_NOCULL=1`). What has NOT been established is whether this capture
contains the character's draws at all. The 69% kill rate is consistent with a
big level where UE3's per-object bounds culling is conservative, and it does not
by itself say a specific object is missing.

The cheap next step is to stop reasoning from aggregates: find the draws whose
world transform is NEAR THE CAMERA (a third-person character is a few hundred
units away, not 15,000) and see whether any exists in the frame. `clip_check.py`
already prints the world position of every draw it is given, so this is one run
over the base pass rather than a new instrument.

### Note (2026-08-06)
## RETRACTED: "NO CHARACTER". The character IS drawn -- it is SHADED PURE BLACK (2026-08-06)

This entry's difference 1 says "Marcus is absent from our frame". That is wrong,
and it sent the search after missing geometry for a day. The character is
present, correctly posed, with the right textures bound, and its base pass
writes ZERO.

### Seen

`scratch/frames/bright.gfr`, presented frame gamma-boosted (0.42) to see into the
shadows: a head, shoulder, upper arm and an outstretched hand in perfect
silhouette on the left of frame, pure black, against a correctly lit corridor.
The shape is unmistakably a COG soldier. Boosting is what makes it visible --
at native exposure it reads as darkness, which is how it was recorded as absent.

### It is the character, established three ways

1. **Bone palette.** `GEARS_DRAW_VS_CONSTS=460` returns **256 vec4s**, and from
   c[8] on they are 3x4 matrices -- rotation rows plus a translation --
   repeating in threes. A skinned skeletal mesh, not static geometry.
2. **Its textures are the character's.** `GEARS_DRAW_TEX_DUMP` + `decode_bc.py`
   on its three bindings: fc0 `0x1e8f000` is a tangent-space NORMAL MAP showing a
   torso, shoulders and vest straps; fc2 `0x1722000` is the ALBEDO -- a bearded
   soldier's face, skin, and dark armour. Both decode correctly. No stub, no
   black texture.
3. **6592 triangles**, the largest skinned mesh in the frame, appearing in six
   draws (depth prepass 177, base pass 460, 655 and 752 on 0x2d0, masked 690,
   738).

### Where it goes black, exactly

`GEARS_DRAW_PIXEL_TRACE` on three separate pixels of the silhouette -- (300,500),
(200,120), (120,300) -- all say the same thing:

    after 280 draws  (4.47, 4.73, 4.46)   <- the scene behind it, lit
    ...
    after 461 draws  (0, 0, 0)            <- draw 460, ps 0xf662d670789bfac0

Draw 460 has `blend_on 0` and `color_mask 15`: it REPLACES, it does not add. So
this is the character's base pass and its output is zero.

### The term that zeroes it

`xenos_translate --raw` on `scratch/shaders/bound/ps_f662d670789bfac0.ucode`,
reduced by hand from the listing:

    r4.x = r2.z / |r2.zxy|              (instructions 4,7,8: dp3, rsq, mul)
    r4.x = saturate(c254.y - r4.x)      (25: subsc_sat, c254.y = 1.0)
    r4.xyz = r5.yxz * r4.x              (26)
    r4.xyz = r4.zxy * r4.w              (27, c6 = 0)
    oC0.xyz = r4.xyz * c254.w           (28, c254.w = 8)

Everything the pass emits is multiplied by `saturate(1 - normalize(r2).z)`. That
is zero wherever `normalize(r2).z >= 1`, i.e. wherever the interpolated vector r2
points straight at the viewer. Ours is apparently doing that over the WHOLE mesh.

### Ruled out, each by measurement

  * **Missing geometry / clipping**: 1431 primitives survive clip and it shades
    144,191 fragments. It rasterises.
  * **Missing interpolators**: the pixel shader reads r1, r2, r4 before writing
    them (`ucode_reduce.py`), and the vertex shader exports o0,o1,o2,o3,o4,o5 --
    `max o1, r12, r12` and `max o4, r2, r2` are full-register writes with no
    mask, which a grep for `o1.` misses. They ARE supplied. (Recorded because
    that grep produced a convincing false lead.)
  * **Missing or black textures**: all three bindings are real guest textures and
    both colour ones decode to recognisable character art.
  * **Constants**: c254 = (0.7, 1, 0.8, 8) and c255 = (0.11, 0.3, 0.59, 0.5) --
    the latter are BT.601 luma weights, so the shader desaturates. c254.w = 8 is
    a BRIGHT multiplier, not a zero.

### What is NOT established, and the next measurement

WHY `normalize(r2).z` saturates the term to zero. r2 is interpolator o2, written
by the skinned vertex shader as `max o2.xyz_, r5.xyzz`. The positions skin
correctly -- the silhouette is a properly posed character -- so the bone matrices
reach the vertex shader intact; something about the TANGENT-FRAME output does
not.

The vertex layout is stride 10 dwords: float3 position, then THREE packed dwords
`[3][4][5]` (0x00a95110, 0x005d09a3, 0x00f36db2 -- note every one has a zero
LEADING byte), two float UVs, a zero, and 0x000000ff. Those three packed dwords
are the normal/tangent/binormal. A byte-order or signedness error in unpacking
them would leave positions perfect and the tangent frame degenerate, which is
exactly the shape of this defect. That is the thing to measure next -- read the
vfetch instructions in `vs_15cbc482459fe5b7.ucode.txt` for those three streams
and check the format and endian we hand the translator against what the guest
declared.
