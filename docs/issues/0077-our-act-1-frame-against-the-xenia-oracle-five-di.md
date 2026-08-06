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

### Note (2026-08-06)
## Two candidate causes for the black character KILLED, and the next tool named

Following the tangent-frame lead from the note above. Both plausible causes are
dead, each by measurement, and both are recorded because each looked convincing.

### KILLED: a byte-order or signedness error unpacking the tangent frame

The previous note proposed this and it is wrong. The character's vertex stride is
10 dwords and the three packed streams are at offsets 3, 4, 5:

    vfetch_mini r9.zyx_, Offset=3, DataFormat=FMT_8_8_8_8, NumFormat=integer
    vfetch_mini r4.xzy_, Offset=4, DataFormat=FMT_8_8_8_8, NumFormat=integer
    vfetch_mini r3.zyx_, Offset=5, DataFormat=FMT_8_8_8_8, NumFormat=integer

`GEARS_DRAW_VDUMP` prints the BYTE-SWAPPED dword, so what it shows is what the
shader sees after Xenia's `EndianSwap32Uint`. Xenia extracts k_8_8_8_8 with
x = bits 0-7, y = 8-15, z = 16-23, w = 24-31. Applying that to real vertices:

    off3: x 16 y 81 z169 w 0 -> (-0.875,-0.365,+0.325)  |v| = 1.0019
    off4: x163 y  9 z 93 w 0 -> (+0.278,-0.929,-0.271)  |v| = 1.0072
    off5: x178 y109 z243 w 0 -> (+0.396,-0.145,+0.906)  |v| = 0.9993

**Every one is unit length**, across every vertex sampled, and the padding lands
cleanly in w. A normal, a tangent and a binormal. The unpacking is CORRECT and
the byte order is right. (Taking the other three bytes gives mean |v| = 1.29,
which is how this was settled rather than argued.)

### KILLED: "the pixel shader's dead interpolator is the bone-index stream"

The pixel shader reads interpolator r4; the vertex shader exports it with
`max o4, r2, r2` at instruction 440; and VS r2 is loaded at instruction 64 from
`vfetch_full r2, Offset=8, FMT_8_8_8_8, NumFormat=integer` -- an attribute that
is 0x00000000 in every vertex dumped. That chain says o4 is zero, which would
make the pixel shader's `mad r5.xyz_, r4.xyzz, c253.xxxx, c253.yyyy` produce a
constant (-1,-1,-1) and collapse everything downstream.

It is wrong: **r2 is written NINE times between instruction 64 and 440**.
Instructions 65-68 immediately spread the fetched value into r0/r1 for the bone
palette lookup (`c[10+a0]`, `c[9+a0]`), after which r2 is reused as an ordinary
temporary. o4 is some computed vector, not the bone indices. Caught before it
was written down as a cause; recorded so the same chain is not walked again.

(Offsets 8 and 9 ARE the bone indices and weights -- indices 0x00000000 and
weights 0x000000ff in the four vertices sampled, i.e. fully weighted to bone 0.
Four vertices of one triangle is not a sample worth concluding from, and the
character IS correctly posed, so skinning works.)

### What is still true, and the tool that is missing

Unchanged: draw 460 is the character's base pass, it replaces rather than blends,
it shades 144,191 fragments, and it writes exactly (0,0,0) on every silhouette
pixel traced. Its output is gated by `saturate(1 - normalize(r2).z)` where r2
descends from interpolator o4.

To find what o4 actually is, that vertex shader's 440 instructions have to be
reduced the way `ucode_reduce.py` reduces a pixel shader. It CANNOT do it: it
models a pixel shader's register file (r0..rN and oC0) and a vertex shader
exports oPos and o0..o15 and indexes a bone palette through the address
register. It used to die on `int('Pos')` with a traceback, which reads as "the
tool is broken" rather than "wrong kind of shader"; it now refuses by name
through its own refusal mechanism and exits 2. Its 7 selftest cases still pass.

NEXT: either extend the reducer to vertex shaders (address register and o-exports
are the work), or read o4 out of the running shader by substituting a native pass
that writes an interpolator as colour -- `runtime/native_pass.{h,cpp}` already
substitutes on a pixel shader hash, so visualising r4 is a small shader and no
new mechanism.

### Note (2026-08-06)
## A third cause killed: the interpolator wiring is correct

`GEARS_DRAW_SPV_DUMP` names each module by its translator MODIFICATION KEY, which
encodes the interpolator mask, so this needed no new code:

    character  vs_15cbc482459fe5b7_mod000000000000003f
               ps_f662d670789bfac0_mod004240000030003f     mask 0x3f  (o0..o5)
    world      vs_cb3cec323318973e_mod000000000000001f
               ps_1f1a3f779667a02a_mod004240000000001f     mask 0x1f  (o0..o4)

0x3f is exactly the six interpolators the character's vertex shader exports, and
it covers r1, r2 and r4 -- the three the pixel shader reads. The mask is
`vs->writes_interpolators() & ps->GetInterpolatorInputMask(...)`, Xenia's
IssueDraw computation verbatim, and it comes out right for this pair. So the
pixel shader is wired to receive what the vertex shader sends.

## Where this leaves the black character

SEVEN candidate causes are now eliminated, each by measurement, and each is
written down above so none is re-run:

  1. missing geometry / clipping -- 1431 prims survive, 144,191 fragments shade
  2. missing interpolator EXPORTS -- o1 and o4 are full-register writes
  3. black or stubbed textures -- all three decode to character art
  4. shader constants -- c254.w = 8, a bright multiplier
  5. tangent-frame byte order -- unpacks to unit vectors, |v| = 1.00
  6. the bone-index interpolator chain -- r2 is rewritten 9x before o4
  7. interpolator mask / modification key -- 0x3f, correct

What remains is the VALUE of interpolator o2 at the pixel, which gates the whole
material through `saturate(1 - normalize(o2).z)`. Nothing measured so far can see
it: it is computed by 440 vertex-shader instructions with control flow and an
address-register bone-palette lookup, which is beyond what `ucode_reduce.py`
models (it now refuses vertex shaders by name rather than crashing).

THE NEXT STEP, scoped: a debug substitution that writes an interpolator out as
colour. `runtime/native_pass.{h,cpp}` already substitutes our own SPIR-V for a
title pixel shader keyed on its hash, and `tools/gen_native_spv.sh` compiles a
`.frag` into the checked-in header -- so this is one small shader
(`oC0 = normalize(r2) * 0.5 + 0.5`, and a second for `saturate(1-normalize(r2).z)`)
plus a roster entry, with no new mechanism. Substituted for
`0xf662d670789bfac0` on `bright.gfr` it answers directly whether o2 is degenerate
and, if so, in which component -- which is the question every eliminated cause
above was a guess at.

### Note (2026-08-06)
## The gate is NOT the cause -- read directly, and it is open

The previous three notes converged on `saturate(1 - normalize(r2).z)` as the term
zeroing the character, having eliminated seven other causes. It is wrong, and now
it is measured rather than reasoned about.

`GEARS_DRAW_DEBUG_INTERP=f662d670789bfac0` (new, `docs/knobs.md`) substitutes a
diagnostic module for that pixel shader which writes the interpolator out as
colour. On `bright.gfr`, pinned to surface 0x400, at two character pixels:

    (200,120)   R 0.317   G 0.841   B 1.000
    (300,500)   R 0.421   G 0.789   B 1.000

  * **R is the gate, and it is 0.32-0.42 -- OPEN.** Not zero, not close to zero.
    Whatever blackens the character, it is not this.
  * G says `normalize(r2).z` = +0.68 and +0.58, consistent with R to three
    decimals (1 - 0.68 = 0.32), so the instrument agrees with itself.
  * **B is saturated at 1.0, so `length(r2) >= 4`.** The interpolator is live and
    large -- not a dead or zeroed vector, which was the other half of the fear.

That is an EIGHTH eliminated cause, and the one every previous note was building
towards.

## What is left in the shader, and it is one multiply

The output chain is `oC0 = ((r5 * gate) * r4.w) * 8`. The gate is open and c254.w
is 8, so blackness has to come from `r5` or `r4.w`:

    18   tfetch2D r5.xyz_, r4.zy, tf1        <- a LOOKUP, sampled at computed coords
    23   mul r5.xyz_, r0.zxyy, r5.zyxx       <- albedo x that lookup
    24   mul r5.xyz_, r5.xyzz, c254.zzyy

`tf1` is base `0x32eb000`, a **256x256 single-channel k_8** texture, and it is
50.8% ZERO and 30.8% at 255 -- a hard-edged ramp, not a smooth one. Its sample
coordinate is `r4.zy`, computed at instructions 14-17 from c3/c4/c5 and c254.
**If the character samples the zero half of that ramp, the material outputs
exactly zero**, which is what every silhouette pixel shows.

That is the next measurement, and the mechanism for it now exists: extend
`runtime/shaders/debug_interpolator.frag` to write the tf1 COORDINATE and its
sampled value instead of r2 (it needs the tf1 binding declared -- see
`uber_post_blend.frag` for the descriptor set/binding convention). A coordinate
outside [0,1], or one landing in the ramp's zero half, names the fault; a
coordinate that looks right moves the search to `r4.w` and the normal map's blue
channel.

NOTE the instrument's own limit: it reports what the SHADER receives, so it
cannot distinguish "the guest asked for this" from "we computed the wrong
interpolator". Both remain possible for r2's length of 4+, which is not what a
normalised tangent-space vector would be -- worth returning to if the tf1 lead
dies.

### Note (2026-08-06)
## Both multipliers healthy, the tf1 binding healthy: it is the COORDINATE

Two more builds of the debug substitution, two more eliminations, and the search
is now down to twelve instructions.

### Ninth elimination: r4.w and the normal map are fine

`oC0.xyz = ((r5 * gate) * r4.w) * c254.w`, and the debug module read r4.w
directly (it is `tf0.z * 2 - 1`, tf0 being the normal map):

    (200,120)   r4.w = 1.000   gate = 0.317   tf0.z = 1.000
    (300,500)   r4.w = 0.916   gate = 0.421   tf0.z = 0.958

With c254.w = 8, `gate * r4.w * c254.w` is about **2.6 -- a BRIGHT multiplier**.
The normal map samples correctly too. So neither the gate nor the normal map nor
the constants can be the zero, and since the material's output IS exactly zero,
arithmetic forces `r5 = albedo * tf1` to be zero. The albedo decodes to real
character art, so **tf1's sample is zero**. That is deduced from measurements,
not assumed.

### Tenth elimination: the tf1 binding works

The obvious next suspect was the binding itself -- tf1 is `k_8`, a
single-channel format with fetch swizzle XXX1, and a mishandled single-channel
texture would read zero everywhere. It does not:

    R = tf1 at a FIXED (0.5,0.5)   0.000 at both pixels
    G = tf1 at r0.xy               0.000 and 0.6567

**G is 0.657 at (300,500)**, so real data reaches the shader through that
binding. The descriptor, the k_8 host format and the XXX1 swizzle all work.

(R being 0 at the fixed probe is NOT a defect and must not be read as one: tf1 is
50.8% zero by measurement, so (0.5,0.5) landing in its zero half is ordinary
texture content. The fixed probe was there to catch a binding that reads zero
EVERYWHERE, and it did not fire.)

### So it is the coordinate, and here is where it is built

`tfetch2D r5.xyz_, r4.zy, tf1` samples at `r4.zy`, built by instructions 11-17:

    11   mul r5.xyz_, r2.xxxx, c1.zyxx
    12   mad r5.xyz_, r2.zzzz, c0.xzyy, r5.zxyy
    13   mad r5.xyz_, r2.yyyy, c2.yxzz, r5.zxyy      <- r2 through the (c0,c1,c2) basis
    14   mul r4._yz_, r5.xxxx, c4.yyxx
    15   mad r4._yz_, r5.yyyy, c3.xxyy, r4.zzyy
    16   mad r4._yz_, r5.zzzz, c5.yyxx, r4.zzyy      <- and through (c3,c4,c5)
    17   add r4.xyz_, r4.xyzz, c254.xyyy             <- BIASED by (0.7, 1, 1)

with, from `GEARS_DRAW_PS_CONSTS` on this draw:

    c0 = (-1, 0, 0, 0)   c1 = (0, -1, 0, 0)   c2 = (0, 0, 1, 0)
    c3 = (-0.9913, -0.0932, 0.0929, 0)
    c4 = ( 0.1024, -0.1031, 0.9894, 0)
    c5 = (-0.0826,  0.9903, 0.1117, 0)     -- both orthonormal
    c254 = (0.7, 1, 0.8, 8)

Instruction 17 adds **+1** to both components that become the sample coordinate.
A sphere/hemisphere lookup normally biases by +0.5 to map [-1,1] onto [0,1]; a
+1 bias sends a [-1,1] input to [0,2], and everything above 1 clamps to the edge.
If the ramp is zero at that edge, every such pixel reads zero -- which is the
symptom exactly.

WHAT THAT DOES NOT ESTABLISH: whether the +1 is the guest's intent (and our r2
is wrong, so the pre-bias value sits in the wrong half) or whether we mis-supply
c254. Both remain open and they call for opposite fixes. The debug module
deliberately does NOT replicate these twelve instructions -- a mis-replication
would produce a confidently wrong answer -- so the next step is to extend it to
output the computed `r4.zy` itself, which is a faithful copy of lines 11-17 and
nothing more.

Also still unexplained and worth returning to if this dies: the debug module
measured `length(r2) >= 4`, where a tangent-space vector should be near 1.

### Note (2026-08-06)
## o4 is healthy too, and a correction to my own reading of the shader

### CORRECTION: the gate and the coordinate come from DIFFERENT interpolators

Earlier notes treated PS `r2` as one thing. It is not: **r2 is OVERWRITTEN at
instruction 10** (`mul r2.xyz_, r5.zxyy, r4.wwww`) before the tf1 coordinate is
built at 11-17. So:

  * the GATE (instructions 4,7,8) reads the INTERPOLATOR o2 -- measured, open;
  * the COORDINATE (11-17) is built from o4, via `mad r5.xyz, r4.xyzz, c253.x,
    c253.y` with c253 = (2,-1) and then a normalise.

The "length(r2) >= 4" recorded two notes ago is therefore about o2, which the
gate only ever uses through `normalize()` -- so it is IRRELEVANT to the failure,
not the loose end it was flagged as. Withdrawn.

### Eleventh elimination: o4 unpacks to a unit vector

    (200,120)   o4.x 0.978  o4.y 0.503   |o4*2-1| = 1.000
    (300,500)   o4.x 0.726  o4.y 0.670   |o4*2-1| = 0.967

Exactly what a packed [0,1] tangent-space vector should give. The unpack
constant is right, the interpolation is right, and the normalise that follows
has a sane input.

### So every input to the coordinate is now measured and healthy

    o4 unpacks to unit          -> r5 = normalize(...) is unit
    r4.w = 0.92..1.00           -> r2 (post-10) is unit
    c0,c1,c2                    -> orthonormal, an exact 180-degree Z rotation
    c3,c4,c5                    -> orthonormal
    c254 = (0.7, 1, 0.8, 8)     -> the bias
    tf1 clamp x=clamp-edge y=clamp-edge, and the guest ASKED for clamp-edge
       (new column on GEARS_DRAW_TEX_BINDS -- we honour it correctly)

A unit vector through two orthonormal bases gives components in [-1,1], and
instruction 17 adds +1 to both that become the sample coordinate -- so the
coordinate lands in [0,2] and roughly half of it clamps to the texture edge. A
sphere-map lookup normally biases by +0.5 for exactly this reason.

### The constants are NOT mis-packed, checked rather than assumed

The obvious explanation is that c254 should be 0.5 and we hand the shader the
wrong vec4. It does not look that way: in the same packed block c253 is exactly
(2,-1,0,0) -- the canonical [0,1]->[-1,1] unpack -- and c255 is exactly
(0.11, 0.3, 0.59, 0.5), the BT.601 luma weights. Both are unmistakable and both
land where the microcode expects them, so the block is aligned and
c254 = (0.7, 1, 0.8, 8) is what the guest really set.

### Where that leaves it

Every measurable input to the failing lookup is correct, and the arithmetic the
guest asks for still sends the coordinate outside [0,1] on much of the mesh. So
the fault is in something NOT yet measurable from our side: either the vector
reaching instruction 11 differs from the console's in SIGN (which would move
[0,2] to [-1,1] and land it correctly under clamp), or the console resolves this
lookup differently than a straight clamped 2D sample.

That is the first point in this investigation where the next step needs the
ORACLE rather than another probe: the same draw's tf1 coordinate as Xenia
computes it. Cross-emulator per-draw comparison is exactly what catalog #84
records as unavailable -- neither wall clock nor guest frame count aligns two
runs, and the fix there is a deterministic guest clock.

ELEVEN causes are now eliminated by measurement, all recorded above. The
instrument built for it (`GEARS_DRAW_DEBUG_INTERP`, docs/knobs.md) reads any
interpolator or texture binding of any pixel shader as colour, and stays.

### Note (2026-08-06)
## MECHANISM PROVEN: interpolator o4 arrives NEGATED, and flipping it lights the character

The debug module now replicates the tf1 coordinate EXACTLY -- not from reading
the listing, but from `tools/ucode_reduce.py`'s straight-line reduction of
ps 0xf662d670789bfac0 (t13..t40), with the constants from
`GEARS_DRAW_PS_CONSTS` on that draw:

    n    = normalize(o4*2-1)          (permuted: t13=n.z, t14=n.x, t15=n.y)
    t7   = tf0.z*2-1                  (the normal map, measured 0.92..1.00)
    t40  = -0.0826*n.z*t7 + 0.9913*n.x*t7 - 0.1024*n.y*t7
    coord.x = t40 + c254.y            (c254.y = 1)

and samples tf1 both as computed and with n.x's sign flipped:

    pixel        ramp as computed   ramp sign-flipped   coord.x
    (200,120)         0.000              1.000           1.972
    (240,180)         0.000              1.000           1.894
    (100,200)         0.000              1.000           1.814
    (300,500)         0.000              0.000           1.330

tf1 (0x32eb000) is a pure HORIZONTAL RAMP -- every row identical, 255 up to
x = 0.31, falling to 0 by x = 0.5, zero after (measured: row means all 99, column
means 255,255,255,255,255,242,142,52,0,0,...). So a coordinate of 1.81-1.97
clamps past the end of the ramp and reads **exactly zero**, and that zero
multiplies the material's entire output. That is the black character, end to end.

**Flipping the sign reads FULL BRIGHT.** The correct coordinate is about 0.1,
the lit end of the ramp; ours is about 1.9. The magnitude is right and the sign
is wrong -- o4 arrives NEGATED relative to what this material expects.

## What this closes and what it leaves

CLOSED: why the character is black. It is not geometry, not textures, not
constants, not the binding, not clipping, not the interpolator mask, not the
gate, not the normal map -- all eleven eliminated causes stand, and this is the
twelfth possibility, confirmed rather than eliminated.

OPEN: which input to the skinned vertex shader is negated. o4 is exported by
`max o4, r2, r2` at VS instruction 440, and r2 there is a computed tangent-space
vector (it is NOT the bone-index fetch from instruction 64 -- r2 is rewritten
nine times in between, recorded above). Candidates, in the order worth testing:

  1. a vertex CONSTANT carrying the eye or light position, negated or in the
     wrong space -- an eye vector computed as (eye - pos) where the guest means
     (pos - eye) flips exactly this way, and costs one sign;
  2. the tangent basis' HANDEDNESS -- the three packed streams at offsets 3,4,5
     unpack to unit vectors (measured) but which is normal/tangent/binormal, and
     the sign of the binormal, is a convention we may have backwards;
  3. our vertex constant upload for this draw more broadly.

(1) is testable without changing the renderer: dump the VS constants for draw 460
-- 256 vec4s, already available through `GEARS_DRAW_VS_CONSTS=460` -- and compare
the camera-related vec4s against the view-projection the frame's other draws use,
which `tools/clip_check.py` already extracts and which is known good because it
places shaded draws inside the frustum and killed ones behind the camera.

THE INSTRUMENT STAYS: `GEARS_DRAW_DEBUG_INTERP` (docs/knobs.md) now carries a
faithful replication of this material's coordinate chain and its sign-flipped
control, so any change to the vertex path can be checked against it in one run.

### Note (2026-08-06)
## CORRECTION: the gate IS the cause. My debug shader was wrong, and it misled three notes.

### The mistake

The debug module computed the material's gate as `saturate(1 - normalize(o2).z)`,
read 0.32-0.42 on the character, and I recorded the gate as OPEN. That is wrong.
`ucode_reduce.py`'s FULL reduction (which I truncated at 60 lines and did not
read to the end) has:

    t42 = (t11 + c254.x)              t11 = normalize(o2).z, c254.x = 0.7
    t70 = saturate((c254.y - t42))    c254.y = 1

so the gate is

    saturate(c254.y - c254.x - normalize(o2).z) = saturate(0.3 - normalize(o2).z)

I dropped the `+ 0.7` by reading the disassembly listing instead of the
reduction -- the exact failure `ucode_reduce.py` exists to prevent, in the exact
way its docstring warns about.

### Measured with the corrected shader

    pixel        REAL gate   normalize(o2).z   the WRONG gate I reported
    (200,120)      0.000          0.68              0.317
    (240,180)      0.000          0.31              0.688
    (300,500)      0.000          0.58              0.421
    (100,200)      0.000          0.69              0.308

**The gate is EXACTLY ZERO at every character pixel.** It multiplies the whole
material output, so this alone produces the black character.

### What this retracts

Three notes above are built on "the gate is open, so look downstream":

  * "Both multipliers healthy, the tf1 binding healthy: it is the COORDINATE" --
    the tf1 and coordinate measurements in it are RIGHT (the ramp really is a
    horizontal gradient, the coordinate really does land at 1.8-2.0, the binding
    really does work), but they are not the cause. With the gate at zero the
    output is zero whatever the lookup returns.
  * "MECHANISM PROVEN: interpolator o4 arrives NEGATED" -- WITHDRAWN as a cause.
    The sign-flip experiment did light the RAMP, but the ramp is downstream of a
    zero. That is also why the two `GEARS_DRAW_PS_CONST_SET` control arms in this
    session changed nothing: substituting the identity for c0/c1/c2, and negating
    c3/c4/c5, both moved the coordinate and neither moved the output, because the
    gate was zero in both arms. Those null results are CONSISTENT with this and
    were the first sign the coordinate story was wrong.
  * "o4 is healthy" stands as a fact about o4 and is now beside the point.

The debug shader now emits BOTH forms -- the real gate in R and the old wrong one
in B -- so this specific error cannot be made again silently.

### Where it actually stands

The character is black because `saturate(0.3 - normalize(o2).z)` is zero, i.e.
because `normalize(o2).z` measures 0.31-0.69 where it must be below 0.3.

NOT YET ESTABLISHED, and these want opposite fixes:
  (a) o2 is wrong -- it is an interpolator whose measured LENGTH is >= 4, which
      is not what a normalised tangent-space vector looks like, so its direction
      may be wrong too; or
  (b) o2 is right and this material is a RIM light that is legitimately near-zero
      facing the camera -- in which case the character's diffuse must come from
      elsewhere, and draws 655 and 752 (ps 0xd113bf9d8354, 112,984 and 164,528
      fragments on surface 0x2d0) are the candidates. They shade large areas and
      were measured NOT to touch the traced silhouette pixels, which is the thing
      to explain first under this reading.

(b) is testable immediately and cheaply: point `GEARS_DRAW_DEBUG_INTERP` at
0xd113bf9d8354 and trace where it writes. If those draws are the character's
diffuse and land somewhere other than the character, that is the real defect and
draw 460 was never going to light anything.

### Note (2026-08-06)
## o2 measured raw: an unnormalised view vector whose Z has the wrong sign

With the gate established as the cause (`saturate(0.3 - normalize(o2).z)`, zero
everywhere on the character), the debug module now reads o2 itself. Un-remapped:

    pixel        o2                              |o2|    normalize(o2).z   gate
    (200,120)    (-23.47, +13.96, +25.48)        37.4        +0.68          0
    (240,180)    (-35.59, +13.69, +12.52)        40.1        +0.31          0
    (300,500)    (-38.75, -36.22, +37.66)        65.2        +0.58          0
    (100,200)    (-23.39,  +6.91, +23.38)        33.8        +0.69          0

Two facts:

  * o2 is an UNNORMALISED vector of magnitude 34-65, not a unit tangent-space
    direction. That is consistent with an eye/view vector in the mesh's own
    units (the mesh's local coordinates run to ~140, per its vertex dump), and
    the shader normalises it itself -- so the magnitude is not by itself a fault.
  * **normalize(o2).z is POSITIVE, +0.31 to +0.69, on every character pixel**,
    and the gate needs it BELOW +0.3.

Flip that sign and the gate becomes `saturate(0.3 + 0.68) = 0.98` -- fully open,
and the character lights. The magnitude is plausible and the sign is not: the
same shape as an eye vector formed as `(eye - pos)` where the material wants
`(pos - eye)`, or a tangent basis with its third row negated.

## Draw 460 really is the character's only textured pass

Worth stating, because it decides whether the gate matters. The other two large
draws of the same 6592-primitive mesh, 655 and 752 (ps 0xd113bf9d83540000,
112,984 and 164,528 fragments, on surface 0x2d0, BLENDED), translate to a module
with **0 textures and 0 samplers**. They cannot be a diffuse pass -- a diffuse
pass samples the albedo. So the character's entire textured appearance comes
through draw 460, and the zero gate blacks all of it.

That closes the alternative reading offered in the correction above ("this is a
rim light and the diffuse is elsewhere"): there is no elsewhere.

## What is left

o2 is exported by the skinned vertex shader at instruction 440
(`max o2.xyz_, r5.xyzz`). Its sign is wrong and everything else about it is
plausible. The remaining question is which vertex-shader INPUT carries that sign
-- a constant holding the eye position, or the handedness of the tangent basis
built from the three packed streams at offsets 3/4/5 (which unpack to unit
vectors, measured, but whose ROLE assignment and binormal sign are a convention).

The instrument reads o2 directly now, so any change to the vertex path is one
run from being confirmed or refuted -- and the shader emits the real gate
alongside the wrong one it used to compute, so the error that cost three notes
cannot recur silently.

### Note (2026-08-06)
## THE CHARACTER RENDERS -- demonstrated, with a control arm (not a fix)

Both closed terms opened together, through `GEARS_DRAW_PS_CONST_SET`:

    f662d670789bfac0:8=-1,1,0.8,8            c254.x 0.7 -> -1, opening the gate
    f662d670789bfac0:3,4,5 = c3,c4,c5 negated, flipping the tf1 coordinate

    character pixel (200,120):  (0,0,0)  ->  (0.365, 0.270, 0.260)

and the frame shows the character -- head, shoulder, upper arm and hand, lit and
shaded, from the very draw that was producing pure black
(`scratch/h38/character_lit.png`).

**THIS IS NOT A FIX AND MUST NOT BE READ AS ONE.** Those constants come from the
guest; overriding them is the control arm this knob exists for ("is the picture
wrong because of THIS number?"). It also overdrives the shading -- c254.x = -1
opens the gate to ~1.0 rather than to a correct value, which is why the face
blows out. What it demonstrates is that the geometry, the skinning, the bone
palette, the textures and the material arithmetic ALL WORK, and that exactly two
terms are closed.

## Why one upstream sign explains both

The two terms look independent and are not:

    gate     = saturate(c254.y - c254.x - normalize(o2).z)   needs normalize(o2).z < 0.3
    coord.x  = t40 + c254.y,  t40 propto normalize(o4*2-1).x  needs t40 < -0.5

o2 and o4 are both built by the vertex shader from the SAME tangent basis --
instructions 323-325 transform a vector by rows held in r5/r7/r11, a rotating
accumulation. Negate that basis and normalize(o2).z flips (gate opens) and t40
flips (coordinate lands at ~0.1, the ramp's bright end) TOGETHER. One sign
accounts for both closed terms; two independent faults would be a coincidence.

That is also why every single-constant control arm in this session moved nothing:
c0/c1/c2 forced to identity and c3/c4/c5 negated each moved ONE of the two, and
the other stayed zero and kept the output at zero. Only opening both shows the
character. Those null results were evidence, not noise.

## The remaining work, precisely

Find which vertex-shader input carries that sign. o2 is exported at instruction
439/440 from r5, last written by instructions 323-325:

    323   mul r5.xyz_, r10.xxxx, r5.xyzz
    324   mad r5.xyz_, r10.yyyy, r7.wyzz, r5.zxyy
    325   mad r5.xyz_, r10.zzzz, r11.zyxx, r5.yzxx

-- r10 through a basis in (r5, r7.wyz, r11.zyx), with ROTATING accumulator
swizzles. That rotation is exactly the construct `tools/ucode_reduce.py` was
built to untangle and exactly the construct that cannot be read off a listing --
which is how this investigation lost three notes to a dropped `+0.7`.

So the next step is the one already named: teach `ucode_reduce.py` vertex shaders
(oPos/oN exports and the address register for the bone palette; it currently
refuses them by name). With the basis reduced, which of r5/r7/r11 is negated --
and whether it traces to a constant or to the packed streams at offsets 3/4/5 --
is readable rather than guessable.

EVIDENCE THAT SURVIVES ALL OF THIS, for whoever picks it up: the character is
drawn, correctly posed, correctly skinned, correctly textured, and one sign away
from being lit. Everything else on this entry's difference 1 is closed.

### Note (2026-08-06)
## The blocker is a REFERENCE VALUE, and neither source of one is available here

Stating this plainly, because the investigation is otherwise complete and the
next session should spend its first ten minutes getting a reference rather than
re-deriving what is already measured.

Everything about the character is now established EXCEPT one number. It is drawn,
posed, skinned, textured and one sign from lit; the gate and the ramp lookup are
both closed and one negated tangent basis accounts for both; and forcing them
open renders the character. What is NOT known is what `o2` SHOULD be. Ours is
(-23.47, +13.96, +25.48) at one pixel, with normalize(o2).z = +0.68 where the
material needs < +0.3.

Three ways to learn the right value, and none is available on this machine today:

1. **The same-frame oracle** -- Xenia's o2 for this draw. Blocked twice over:
   trace playback renders colour black (catalog #79) and neither wall clock nor
   guest frame count aligns two live runs (catalog #84, which needs a
   deterministic guest clock).
2. **UE3's own sources** -- `GEARS_UE3_SRC`, which `docs/native-renderer.md`
   uses to write native passes from the engine rather than from our reading of
   microcode. UNSET, and no checkout exists on this machine (searched).
3. **Reducing the vertex shader symbolically** -- ruled out, not merely
   unavailable: this shader has predication and jumps (instructions 315-328 are
   inside `(p0)` blocks with `jmp L83` / `jmp L114`), and `ucode_reduce.py`
   REFUSES predication and control flow by design. Extending it to vertex
   shaders would not help; the refusal is about the control flow, not the stage.

## What must NOT be done

Patching the guest's constants to open the gate. That is what
`GEARS_DRAW_PS_CONST_SET` did to produce `scratch/h38/character_lit.png`, and
that knob's own documentation says it: "It is never a fix: the number comes from
the guest, and a wrong one is a bug on the CPU side, not here." Shipping it would
hardcode a value to make one frame look right and would be exactly the class of
change this project's rules forbid. The image is evidence, not a patch, and the
tree contains no such change.

## The cheapest route to unblocking, in order

  * A UE3 checkout at `GEARS_UE3_SRC`. This material is a skinned base pass with
    a `saturate(0.3 - N.V)`-shaped term and a 1D ramp lookup; the engine source
    says what vector feeds it and in which space, which settles the sign without
    any emulator comparison. Cheapest by far if the source can be obtained.
  * Otherwise catalog #84's deterministic guest clock, which unblocks per-draw
    cross-emulator comparison generally -- not just this question.

### Note (2026-08-06)
## UE3's SOURCE SAYS OUR o2 IS CORRECT. The "negated basis" hypothesis is dead.

With a UE3 checkout available (`GEARS_UE3_SRC`, see docs/native-renderer.md), the
question that had no reference now has one, and it goes against the last three
notes.

`Engine/Shaders/LocalVertexFactory.usf:276` -- and, for this mesh, the SKINNED
factory at `Engine/Shaders/GpuSkinVertexFactory.usf:244`, identically:

    Result.TangentCameraVector = TransformWorldVectorToTangent(
        Result.TangentToWorld,
        CameraPositionDS.xyz - CameraLocalWorldPosition.xyz * CameraPositionDS.w);

So UE3's tangent camera vector is:

  * **(CameraPosition - WorldPosition)** -- pointing FROM the surface TO the
    camera, so its tangent-space z is POSITIVE on a surface facing the viewer;
  * **UNNORMALISED** -- a difference of positions, so its magnitude is a
    distance.

Ours measures (-23.47, +13.96, +25.48) with |o2| = 34-65 and
normalize(o2).z = +0.31..+0.69. That is exactly the sign and exactly the shape
UE3 specifies. **o2 is right.**

### Consequences, and they are large

  * "o2 arrives negated" and "one upstream sign closes both terms" are WITHDRAWN.
    The demonstration image `scratch/h38/character_lit.png` remains a true
    statement that forcing the terms open renders the mesh, and a false one about
    the cause.
  * `saturate(0.3 - normalize(o2).z)` is therefore MEANT to be ~0 on surfaces
    facing the camera. Draw 460 is a RIM / edge term, and it is CORRECTLY dark
    over the body. It was never the character's diffuse.
  * So the character's diffuse contribution is genuinely ABSENT from this frame's
    textured draws, which is a different defect from the one chased all session:
    a missing or mis-attributed draw, not a wrong value.

### Also tested and NOT the cause: the EDRAM tiling collapse

The collapse drops real draws -- "1 tile group(s) collapsed, 185 replayed draws
and 2 resolves dropped", 641 of 844 issued -- so it was a good suspect for a
missing diffuse pass. `GEARS_DRAW_TILED=1` restores them (826 of 844 issued) and
adds a write to the character pixel at draw 534, but the presented frame is
statistically identical (R mean 0.0492, p99 0.408, 16.1% black in both arms) and
the character is still black. Not the cause.

### Where difference 1 actually stands now

The character mesh (6592 primitives, bone palette, real character textures) is
drawn six times in this frame, and NONE of those draws is a lit diffuse pass:

    177  depth prepass, 0 fragments
    460  base pass, 144,191 fragments -- the RIM term, correctly ~0
    655  0x2d0, 112,984 fragments -- module has 0 textures, 0 samplers
    690  0x2d0, colour mask 0
    738  0x2d0, 0 fragments
    752  0x2d0, 164,528 fragments -- same 0-texture module as 655

Next: find what the guest's diffuse pass for this mesh looks like and whether it
is in the capture at all. `Engine/Shaders/BasePassPixelShader.usf` and
`MaterialTemplate.usf` now say what a lit UE3 base pass binds and outputs, which
is the reference this question lacked. If no such draw exists in the stream, the
defect is upstream of the renderer entirely -- and that is a CPU-side question,
not a GPU one.

### Note (2026-08-06)
## WITHDRAWN: "characters CAN render correctly" (2026-08-06)

A note added to catalog #85 earlier today claimed the difficulty-select preview
proved characters render correctly somewhere, and used that to constrain this
entry. It does not. `ingame_v3.gfr` is a MENU frame -- 159 draws, largest mesh 92
primitives, no bone palette anywhere -- so its soldiers are a texture, not
rendered geometry.

This entry is exactly where it was: the only skinned character draw measured in
any capture is bright.gfr's draw 460, and it is black. Nothing has been shown
about whether the renderer can light a character, because no other capture
contains one to try.

That is worth stating plainly for the next session: **there is no working
character render to A/B against.** Getting one -- a capture whose frame contains
a lit skinned mesh -- would be worth more than any further analysis of draw 460,
because it turns an open question into a differential one. `tools/oracle_lockstep.sh`
and `tools/capture_gameplay_frame.sh` both reach gameplay; a capture taken where
the camera clearly shows the player would do it.

### Note (2026-08-06)
## Attempt to capture a character frame: the walk does not reliably show one

Acting on the note above -- "there is NO working character render to A/B
against, getting one is worth more than further analysis" -- I captured a fresh
gameplay frame with stick input driving the camera
(`GEARS_INPUT_SCRIPT=...,135000:LY+,160000:RX+,175000:LY+`, dumped at guest
frame 5400). Saved as `scratch/frames/wallcorner.gfr`.

It does NOT contain a character. 350 draws, and its only meshes above 1000
primitives are 2302-primitive draws whose vertex shader declares 16 constants
with no bone-matrix rows -- static geometry. The frame is a wall corner: the
scripted walk pushed the camera into a wall.

Recorded so the next attempt does not repeat it. The scripted walk reaches
gameplay reliably but does NOT reliably frame the player, and a capture is only
useful here if a skinned mesh is actually on screen. Two ways to make that
deterministic rather than lucky:

  * check the capture before using it -- a frame containing a character has a
    mesh of several thousand primitives whose VS constants include bone-matrix
    rows (unit xyz, large w). That check is three lines of the diag table plus
    `GEARS_DRAW_VS_CONSTS`, and it is what this note used to reject this capture
    in under a minute;
  * or drive the camera away from walls: `RX-`/`RX+` swings the view, and a
    capture taken while the camera is turning is far more likely to frame the
    player than one taken mid-forward-walk.

`bright.gfr` remains the only capture in the tree containing a skinned character
draw, and it is the one where that draw renders black.

### Note (2026-08-06)
## Three capture attempts, no character frame. The walk cannot frame the player.

Following the plan above -- get a working character render to A/B against -- I
made three attempts. None produced a frame containing a skinned character, and
the reason is now clear enough to stop trying this way.

  1. stick-forward walk, dumped at guest frame 5400 -> a WALL CORNER. 350 draws,
     largest mesh 2302 primitives with 16 vertex constants and no bone rows.
     Saved as `scratch/frames/wallcorner.gfr`.
  2. camera-turn walk, dumped at guest frame 5200 -> the run hit catalog #44's
     nondeterministic crash before the dump. No file.
  3. same walk, retried -> SUCCEEDED as a capture and is a much richer gameplay
     frame (477 draws, a prison interior with pillars and blood decals, and
     visible MOTION BLUR, so that pass is working). Saved as
     `scratch/frames/prison_turn.gfr`. But its four meshes above 3000 primitives
     are all killed by clip or colour-masked, and no character is on screen.

WHY THIS KEEPS FAILING, and it is not bad luck: Gears' third-person camera sits
BEHIND the player, so during a forward walk or a camera turn the player is
frequently out of frame or hard against a wall. A capture that frames the
character needs the player STOPPED and the camera swung to bring them into view,
or a scripted moment that does it (a cover-take, a roadie-run stop).

`bright.gfr` is still the only capture in the tree with a skinned character draw.
It was captured before this session and I do not know what walk produced it.

## What this costs, and the cheaper alternative

Each attempt is about four minutes and one in three ends in #44's crash, so
this is an expensive coin flip. Two better routes:

  * make the capture SELF-SELECTING -- have `capture_gameplay_frame.sh` keep
     dumping until the frame it dumps contains a mesh with bone-matrix vertex
     constants, and stop then. The test is the three-line check this entry
     already used to reject attempt 1, and it turns a coin flip into a loop.
  * or work `bright.gfr` from the CPU side instead: find what emits its draw 460
     and why no lit diffuse pass accompanies it. CodeRed-Generator (see
     docs/native-renderer.md) recovers UE3 class layouts and is the standard
     route to that, and it is also what catalog #58 has been stuck on.

`prison_turn.gfr` is worth keeping regardless: it is the richest gameplay frame
in the tree and the first that visibly exercises motion blur.

### Note (2026-08-06)
## The tree already had four character captures, not one. And the character's colour-writing draws die at CLIP.

The three failed capture attempts above were followed by the cheaper route
they recommended: make the capture SELF-SELECTING. That is now built, and
running it corrected two things this entry believed.

### The detector, and why it is not a heuristic over constant values

A UE3 skinned mesh transforms each vertex by bone matrices fetched from a
palette of float constants indexed by the vertex's own blend indices, which
compiles to a float-constant read through the address register (a0). Rigid
geometry has no reason to index constants dynamically. So "is this a skinned
mesh" is answered by Xenia's ucode analysis
(`constant_register_map().float_dynamic_addressing`), not by guessing which
constant rows look like a matrix -- which is what the by-hand check did.

  * `runtime/frame_content.{h,cpp}` -- the scan and its census
  * `GEARS_SKINNED_CHECK=1` (frame_replay) -- exit 0 found / 3 none / 2 no
    translator; `GEARS_SKINNED_CHECK_LIST=1` lists every skinned draw
  * `tools/skinned_frames.sh` -- the table; `--selftest` runs BOTH classes
    (bright.gfr must be FOUND, courtyard.gfr must be NONE) and passes
  * `GEARS_DRAW_FRAME_DUMP_SKINNED=1` -- the runtime gate

### Correction 1: bright.gfr was NOT the only capture with a character

Over all 15 captures in the tree: **4 contain skinned character draws** --
bright, black, play_v2 and prison_turn -- not one. Eleven do not, including the
744-draw gameplay frames act1, courtyard, walk_gameplay and walk_v3, so this is
not "any big frame passes".

prison_turn.gfr in particular was rejected by the note above as containing no
character. It contains **17 skinned draws across 4 distinct skinned shaders**,
including three 30720-index meshes. The earlier rejection looked at the largest
meshes and their clip verdicts, saw them all killed or masked, and concluded
there was no character on screen. The character is there; its draws die.

### Correction 2: the walk CAN frame a character -- the timing was the problem

One 260-second run with the gate on scanned **2614 frames** and captured the
first one that submitted a character, at guest frame 2913:
`scratch/frames/character_auto.gfr` -- 750 draws, 12 skinned draws, 5 distinct
skinned shaders, meshes up to 36732 indices, two actors. The walk was never the
obstacle; choosing the frame by wall-clock was.

### What the captures then show, and it is a lead

Joining the skinned draws against the diag table, counting colour-writing
(color_mask != 0) skinned draws by whether anything survived clip:

    capture          colour-writing: survive / killed at clip   masked, surviving
    bright                 3 / 0                                       6
    black                  3 / 0                                       6
    play_v2                2 / 13                                      9
    prison_turn            0 / 9                                       5
    character_auto         0 / 3                                       5

In three of five captures EVERY colour-writing character draw has
`prims_after_clip = 0`, while the SAME actor's colour-masked draws (the depth
and velocity passes) rasterise thousands of primitives in the same frame.

That the two are the same actor is measured, not assumed:
`GEARS_DRAW_VS_CONSTS=492,520` on character_auto shows draws 492 (10292 prims,
mask 15, 0 after clip) and 520 (10292 prims, mask 0, 4306 after clip) carrying
**identical bone rows** -- same skeleton, same pose, same frame. They differ in
vertex shader (0xd1f8fda33c3a18cc vs 0x8354e5cc00c0a98c) and constant layout.

So "the player is off screen" cannot explain the kill: the geometry is on
screen for the pass that does not write colour. And bright.gfr, the one capture
where a colour-writing character draw DOES survive clip, is the one where it
renders black. Between them those are the two shapes this entry has to explain,
and the second one now has a stage attached to it: clip, not shading.

Next: root-cause the clip kill on character_auto draw 492 (10292 prims in, 0
out) against draw 520, which survives with the same pose. The two shaders'
position transforms are the thing that differs.

### Note (2026-08-06)
## WITHDRAWN, same day: "the colour-writing character draws die at clip while the same actor survives elsewhere"

The note above ends with a lead, and the load-bearing half of it is wrong. I am
recording it rather than quietly editing, because the shape of the mistake is
the one this entry has now made twice.

**The surviving draws are SHADOW MAPS.** `tools/pass_structure.py` on all four
character-bearing captures puts every colour-masked skinned draw that survives
clipping -- character_auto 520/538-543, prison_turn 519-523, play_v2 641-643,
bright 690/693-695 -- inside the depth-only block that feeds a
`depth 0x0 -> 0xc520000` resolve (448x448, 864x864, 864x672 ...). Those are
renders from the LIGHT's point of view, not the camera's.

So "the geometry is on screen for the pass that does not write colour" is false.
A mesh inside the light frustum says nothing whatever about the camera frustum,
and "the player is off screen" remains a perfectly good explanation of the
colour-pass kill. Claim C015 is falsified; its surviving half (4 of 15 captures
submit a character) is re-recorded as C016.

**This is catalog #74's mistake again.** There the same shape -- "instances of
one mesh, some of them wrongly clipped" -- was retracted after the vertices were
actually pushed through the transform and found to be genuinely outside the
frustum. The clip verdict alone never distinguishes "our clip is broken" from
"UE3 submitted geometry that is off camera"; only transforming the vertices
does.

### What would actually settle it, and why it is not a five-minute check

`tools/clip_check.py` does exactly this for RIGID geometry: it takes
`GEARS_DRAW_VDUMP` + `GEARS_DRAW_VS_CONSTS`, treats c0..c3 as the world matrix
and c7..c10 as the view-projection, and calibrates itself on a draw the GPU
demonstrably rasterised. That layout does not hold for a SKINNED draw: on
character_auto draw 492 the bone rows start at c0, and on draw 520 they start at
c5 behind a 4x3 matrix and a c4=(4,4,4,4). Pointing the existing tool at a
skinned draw would read bone rows as a view-projection and produce confident
nonsense -- though its calibration arm should refuse first, which is worth
verifying before anyone tries.

The honest next step is therefore RE, not a diag join: find, in the skinned
vertex shader's microcode, which constants carry the view-projection and how the
bone palette is indexed, then transform the dumped vertices through the skinning
the shader actually performs. Until that is done, this entry has NO evidence
that any character draw is wrongly clipped.

### What is left standing from the note above

  * 4 of 15 captures submit a skinned character (C016), and prison_turn --
    rejected by hand last session as containing none -- has 17 skinned draws.
  * The detector, its self-test and the self-selecting capture gate.
  * bright.gfr remains the one capture where a colour-writing character draw
    survives clipping (draw 460, 1431 of 6592 primitives, 144191 fragments),
    and it is the one that renders black. That, not the clip counts, is still
    the frame to work.

### Note (2026-08-06)
## ANSWERED: the character in character_auto.gfr is BEHIND THE CAMERA. The clip is correct.

The withdrawal above left the question open -- "this entry has NO evidence that
any character draw is wrongly clipped" -- and named the RE needed to settle it.
That RE is done, and the answer is measured in both directions.

### The skinned vertex shader's transform chain, read out of its microcode

`xenos_translate --raw` on vs `0x15cbc482459fe5b7` (bright.gfr's character draw
460, and character_auto's draw 319 -- the same shader):

    instr 60,196   the fetched position is rebuilt as r10 = (pz, px, py, 1)
    instr 197-200  skinning: dp4 against c[8+a0], c[9+a0], c[10+a0] -- a bone
                   palette of three rows per bone from c8, so 82 slots in the
                   256-constant block; a character uses about 45 and the rest
                   are left ALL-ZERO
    instr 202-205  world:  r11 = x*c0 + y*c2 + z*c1 + c3
    instr 207-210  clip:   oPos = x*c233 + y*c234 + z*c235 + w*c236
    instr 435      oPos exported from r12

**The view-projection is at c233..c236**, not the c7..c10 that rigid draws use.
That alone is why `clip_check.py` could never have answered this.

**Every swizzle in the chain cancels, and that is not a coincidence.** The
accumulator is rotated by `.wyxz` at each step; call that permutation
P = (3,1,0,2). It fixes one component and 3-cycles the other three, so P applied
three times is the IDENTITY -- and each constant's own swizzle is exactly the
inverse of the rotation its term receives before landing. Implementing the
rotations literally transposes both matrices into nonsense. This is the trap the
`ue3-native-pass` skill warns about, met head on.

### The measurement, with a control arm in both directions

`tools/skeleton_where.py` (new) transforms each bone's ORIGIN through world and
view-projection and reports where the joints land. It refuses any shader whose
layout has not been read out of its microcode, and it refuses to report anything
at all unless a calibration draw -- one the renderer says it SHADED -- comes back
with a majority of its skeleton on screen.

    bright.gfr draw 460      RASTERISED 1431 of 6592 prims, 144191 fragments
      44 of 45 joints ON SCREEN, 0 behind
      ndc.x -1.16 .. +0.11   ndc.y -0.68 .. +0.98      <- a character in frame

    character_auto.gfr draw 319   killed_by_clip_or_cull, 0 of 6592 prims
      43 of 44 joints BEHIND THE CAMERA, 0 on screen
      the one remaining joint at ndc.x +3.86             <- four screens right

**The clipping is correct.** The player is behind the camera in that frame, and
"the player is off screen" was the right explanation all along. This is the third
time on this entry that a clip verdict has looked like a defect and turned out to
be geometry the guest genuinely put off camera (see #74's retraction); the
difference is that this time there is a number attached.

### Two traps this measurement had to survive, both recorded in the tool

  * **Unused palette slots are not joints.** 37 of the 82 slots are all-zero; a
    zero matrix maps the origin to the world translation, a single point that
    may well be on screen. Counting them put 9 phantom joints on screen for a
    skeleton that has none there.
  * **The first calibration gate was too weak.** It asked for "at least one
    on-screen joint", and a layout that was genuinely wrong passed it with 1 of
    45 while scattering the rest across 345 screens. It now requires a majority.
    The wrong layout was caught by that gate, not by inspection.

### What this does NOT settle

Only vs `0x15cbc482459fe5b7` has a known layout. The killed character draws in
prison_turn and play_v2 use different skinned shaders (0x455aa697b9d60993,
0xd1f8fda33c3a18cc, 0xbff17775a314aa7a, 0x8354e5cc00c0a98c) and the tool refuses
them by design. And this measures the SKELETON, not the mesh, so a character
straddling the frustum edge is not decidable this way -- 43 joints behind the
camera is.

**bright.gfr draw 460 remains the frame to work**: a character demonstrably in
frame, rasterising 144191 fragments, rendering black.

### Note (2026-08-06)
## Two of the six character draws are ELIMINATED: they are constant-black by construction

The list of six character draws above marks 655 and 752 as "module has 0
textures, 0 samplers", which reads as a lost or mistranslated diffuse pass.
It is not. Their pixel shader, `0xd113bf9d83540000`, is THREE INSTRUCTIONS in
full:

    alloc colors
    exece
    sgt oC0.xyz0, -r_abs[5].xxxx, c255.xxxx

`sgt` is set-greater-than, so oC0.xyz = (-|r5.x| > c255.x) ? 1 : 0, and the `0`
in the `xyz0` write mask puts a literal zero in alpha. `-|r5.x|` is never
positive, and the shader packs exactly one vec4 with c255 = (0,0,0,0)
(`GEARS_DRAW_PS_CONSTS=d113bf9d83540000`), so the comparison is false for every
pixel of every draw.

**Those draws write pure black with zero alpha because that is what they are
written to do.** They have no textures because they sample nothing. No renderer
change can affect them, they are not a candidate for the missing diffuse pass,
and their 112,984 and 164,528 fragment invocations mean nothing.

That leaves the six-draw list looking like this:

    177  depth prepass, 0 fragments                     -- not a colour pass
    460  base pass, 144,191 fragments, ps f662d670789bfac0  <- THE ONLY CANDIDATE
    655  constant black by construction                 -- ELIMINATED
    690  colour mask 0                                  -- writes no colour
    738  0 fragments                                    -- nothing rasterised
    752  constant black by construction                 -- ELIMINATED

### And draw 460's shader is a rim term, which its own arithmetic confirms

`0xf662d670789bfac0` (90 dwords) samples three textures and shades them, then
ends:

    25   subsc_sat r4.x, c254.y, r4.x     <- saturate(c254.y - r4.x)
    26   mul  r4.xyz, r5.yxzz, r4.xxxx    <- the shaded colour TIMES that scalar
    27   mad  r4.xyz, r4.zxyy, r4.wwww, c6.xyzz
    28   mul  oC0.xyz, r4.xyzz, c254.wwww

and r4.x at instruction 25 traces back to instructions 4-8, `rsq` of
`dp3(r2.zxy, r2.zxy)` scaled by r2.z -- i.e. the z component of a NORMALISED
interpolated vector, which is an N.V term. `saturate(1 - N.V)` is a Fresnel/rim
factor: ~0 across a surface facing the camera, ~1 only at silhouette edges.

So this pass is *supposed* to contribute almost nothing where the character
faces you. It renders black because it is a rim pass with nothing underneath it.

**The conclusion this entry already reached is now much harder to avoid: the
character's lit diffuse pass is NOT IN THE FRAME AT ALL.** Of six draws, two are
constant-black by construction, one is depth-only, one is colour-masked, one
rasterises nothing, and the last is a rim term. There is no draw here that could
put a lit character on screen, so nothing the RENDERER does to these draws will
produce one. The defect is upstream, on the CPU side, exactly as this entry
suspected -- and that makes it the same class of problem as catalog #73 and #58.

### Note (2026-08-06)
## Ground truth restored, four renderer causes eliminated, and one number that may explain all five differences

Working the goal "fix the game", I went at the black character directly. I did
not fix it. What follows is what was measured, because four of these are
eliminations and the last is a lead worth more than the four.

### The oracle works, and the oracle frames IN THE TREE are a trap

`scratch/oracle/frames_long/` holds 11 PNGs from a previous run. **All eleven
are 100% black, max value 0.** Anyone comparing against them would be comparing
against nothing. They should be deleted or regenerated.

A fresh run works fine: `xenia_oracle --target=$GEARS_ISO --oracle_seconds=240
--oracle_interval=30 --oracle_input="START@25+8,A@30+2"` produced 8 frames,
means 17.7-22.0 with 14k-34k distinct colours -- matching the numbers this entry
opened with. `scratch/oracle/probe/frame_0120s.png` is a proper gameplay frame:
Marcus crouched behind cover, armour lit and detailed, three windows with BARS
and light shafts through them, fine wall texture.

**So difference 1 is confirmed real**: the character is meant to be clearly lit
and visible. Ours is a black silhouette (`bright.gfr` and `black.gfr` both).

### Four renderer-side causes for the black character, eliminated by measurement

  1. **The interpolator is healthy.** `GEARS_DRAW_DEBUG_INTERP=f662d670789bfac0`
     renders o2 directly: a smooth, geometrically coherent field over Marcus's
     head, shoulder and arm. It is not zero and not garbage.
  2. **The vertex fetch is correct.** `GEARS_DRAW_VDUMP=460` gives the packed
     8-8-8-8 tangent frame at dwords 3-5; `0x00a95110` decodes to (16, 81, 169),
     and v*2-1 gives (-0.874, -0.364, 0.325) with length **1.0016**. A unit
     vector. Vertex 1 gives 1.004. The normals are being fetched and decoded
     right.
  3. **Shader binding is clean.** The shader-load report is `lucent::debug` and
     so silent by default; with `GEARS_LUCENT_DEBUG=gpu` a 90 s run reports
     **346,324 IM_LOAD + 61,142 IM_LOAD_IMMEDIATE, 0 rejected, 0 truncated**. No
     load is being dropped, so no draw is inheriting a stale shader.
  4. **The base pass really is a rim term.** Its constants are now read rather
     than inferred: c254 = (0.7, 1, 0.8, 8), so instruction 25's
     `subsc_sat r4.x, c254.y, r4.x` after instruction 17's `+c254.x` is
     `saturate(0.3 - N.V)` exactly. And c254.w = 8 is the final multiplier --
     NOT a zero scale, so this is not catalog #73's failure mode.

Together with the earlier elimination of draws 655 and 752 (constant-black by
construction, and the guest itself loaded that 3-instruction shader), there is
no renderer-side candidate left in this frame.

### THE LEAD: our guest issues about a THIRD of the draws the oracle's does

The oracle logs `gears_draws_recorded_` per frame -- incremented once per draw
actually recorded into the command buffer, `vulkan_command_processor.cc:3159`,
with dropped draws counted separately. At gameplay it reports:

    oracle, gameplay frames:   2295, 2296, 2297, 2359, 2362, 2363, 2364, 2365

Our own gameplay captures, counting the DRAW_INDX packets the guest issued
(before any collapse of ours):

    act1 737   bright 844   black 828   courtyard 744
    play_v2 868  walk_gameplay 744  prison_turn 622  character_auto 750

**Two tight clusters that do not come close to overlapping: 622-868 against
2295-2365, a factor of ~2.8.** Both sides include the console's per-tile command
buffer replays, so that is not the difference.

If our recompiled guest is submitting a third of the scene, it would explain
difference 1, difference 2 and difference 3 with ONE cause rather than three:
the character's lighting passes, the window bar geometry and the HUD would all
simply never be submitted. That reframes this entry from five renderer faults to
one CPU-side deficit.

WHAT WOULD FALSIFY IT, and it must be checked before anyone acts on it: the two
sides are at DIFFERENT MOMENTS, and although the within-side spread is small
(±15% on both), no one has yet put our runtime and the oracle at the same moment
and counted. The way to do that is a scripted walk both sides reach gameplay on
-- note that the oracle's `START@25+8,A@30+2` spam does NOT get our runtime into
gameplay (measured: 4831 frames, never above 188 draws, i.e. still in menus),
so `tools/capture_gameplay_frame.sh`'s walk is the one to port to the oracle
rather than the reverse.

### Note (2026-08-06)
## WITHDRAWN: "our guest issues a third of the draws the oracle's does". It issues MORE.

The note above offered a draw-count deficit as the single cause that might
explain the missing character lighting, window bars and HUD together. It is
wrong, and it was wrong in the same way as this session's earlier retraction:
two numbers taken from DIFFERENT GAME MOMENTS and compared as if they were the
same one.

### Measured, on the runtime, at gameplay

Four paths in `CaptureFrameDraw` discard a draw and NONE of them was counted, so
"this frame had 800 draws" could not be distinguished from "this frame had 2400
and we kept 800". They are counted now, and reported per frame with the
denominator. On a scripted gameplay run:

    frame has 3936 draws of 3936 the guest issued
      (dropped: 0 no shader pair, 0 zero indices, 0 immediate-index,
                0 after frame done)
    frame has 3928 draws of 3928 ... 0 0 0 0
    frame has 3911 draws of 3911 ... 0 0 0 0

**3936 draws a frame, against the oracle's gameplay median of 2141. We issue
MORE, and we drop NOTHING.** The 622-868 range quoted above comes from the
captures in `scratch/frames/`, which are single frames from lighter moments; the
live peak is four to six times that. There is no draw deficit.

Also eliminated on the way, so nobody repeats them:

  * **Predication is not dropping draws.** Ours is `(binSelect & binMask) != 0`,
    byte-identical to Xenia's `pm4_command_processor_implement.h:411`, and the
    per-frame census reports DRAW_INDX skip 0.
  * **No unhandled draw opcode.** The IB census's `op0x27` and `op0x2b` are
    IM_LOAD and IM_LOAD_IMMEDIATE, which we do handle -- they simply have no
    entry in `OpcodeName()` and print as raw numbers.
  * **Frame pacing is not a confound either way**: the oracle presents 7120
    frames in 240 s (29.7 fps), ours 4831 in 170 s (28.4 fps).
  * **Nor is EDRAM tiling**: the oracle resolves a median of 18 times per
    gameplay frame, ours 16. A side replaying the command buffer over more tiles
    would resolve proportionally more often.

### What survives

The instrumentation, which is worth keeping on its own: four silent data-loss
paths in the draw capture now carry counters and print with their denominator,
so a frame that loses geometry there can never again look like a frame that had
none to lose.

And the elimination stands: with no draw deficit, no dropped draws, a healthy
interpolator, a correct vertex fetch, clean shader binding and a base pass whose
rim gate is genuinely ~0, there is still no renderer-side explanation for the
black character in `bright.gfr`.

### Note (2026-08-06)
## Two of the five differences do not reproduce on the LIVE game (2026-08-06)

Everything on this entry has been argued from `.gfr` captures. Driving the real
runtime through the menu walk with `GEARS_DRAW_FRAME_REPORT_EVERY=250` and
looking at the frames it actually renders gives a different picture from the one
this entry has been carrying.

`scratch/live_shots/frame_04750.ppm` (mean 20.1, 5697 distinct colours): a
prison wall, moss line, motion blur, and a small window at the upper left
**showing its BARS as a clear cross pattern**. Difference 3 says "windows are
flat grey blocks"; here the bar geometry renders.

`scratch/live_shots/frame_04500.ppm`: an objective/subtitle panel at the top and
**a HUD element at the bottom centre**. Difference 2 says "no HUD"; something of
the HUD is drawing.

Neither observation clears the renderer -- these are different moments from the
one the original comparison used, which is exactly the trap this entry has now
fallen into three times. What they do establish is that **differences 2 and 3 are
not systemic**: the passes that draw window bars and HUD elements do run and do
produce pixels. Whatever is wrong at the compared moment is narrower than "the
pass is missing", and re-comparing at a matched moment is now the cheapest way
to make progress on both.

Peak live draw counts on that run: **4142 kept of 4142 issued**, zero dropped on
all four paths.

Difference 1 (the black character) is NOT in this category: it reproduces on
every capture that contains a character, and the oracle frame captured today
shows Marcus clearly lit. That one is real and still unexplained.

### Note (2026-08-06)
## The black character is localised to ONE texture fetch: the env-lighting ramp lookup

Previous notes established that no renderer-side cause could be found. That was
because nobody had made the shader's own arithmetic observable. Forcing its
constants one at a time does exactly that, and the answer is now narrow.

### The draw OVERWRITES the pixel with black; it does not merely fail to add

`GEARS_DRAW_SURFACE=0x400 GEARS_DRAW_PIXEL_TRACE=150,300` on bright.gfr:

    after 386 draws = (0.017837, 0.016708, 0.019882, 1)  <- draw 385
    after 461 draws = (0, 0, 0, 1)                        <- draw 460, the character

Draw 460 is opaque and writes zero. So the question is exactly "why is this
shader's output zero", not "which pass is missing".

### The output reaches the screen, so nothing downstream is discarding it

`GEARS_DRAW_PS_CONST_SET='f662d670789bfac0:6=1,1,1,1'` forces c6, the additive
term of instruction 27 (`mad r4.xyz, r4.zxyy, r4.wwww, c6.xyzz`), which is
normally (0,0,0). The character becomes a **solid white silhouette** covering his
whole body. Depth, blend, colour mask and the pass itself are all fine.

### It is a TEXTURE SAMPLE that is zero

`GEARS_DRAW_NOTEX=1` replaces every texture with a white stub. The character
region (x 60-240, y 250-520) goes from **max 0 to max 175, mean 9.7**. With real
textures the same region is exactly zero except a handful of texels -- confirmed
by raising the shader's final multiplier c254.w from 8 to 2000, which lights only
a few isolated specks. So the product is a true zero, not a small number.

Of the three textures the shader samples, two are healthy:

    fc0  0x1e8f000  k_DXT1 256x256  the NORMAL MAP -- decodes to a correct
                                    tangent-space map (neutral purple-blue, no
                                    channel swap), tool: tools/decode_bc.py
    fc2  0x1722000  k_DXT1 256x256  diffuse, 97.8% non-zero
    fc1  0x32eb000  k_8    256x256  THE ENV/LIGHTING RAMP

### The ramp, and why our lookup lands in its black half

`fc1` is a horizontal gradient: **white for u below about 0.3, a short ramp, then
black for the remaining two thirds.** 49% of its texels are zero, uniformly by
row, because the gradient runs along u.

The shader samples it at instruction 18, `tfetch2D r5.xyz_, r4.zy, tf1`, so
u = r4.z. Instructions 14-16 build r4.yz by transforming the normal through the
orthonormal rows in c3/c4/c5 -- note the accumulator swizzle `r4.zzyy` swaps the
two components at every step, so the terms pair up as
r4.z = r5.x*c4.x + r5.y*c3.x + r5.z*c5.x -- and instruction 17 then adds
c254.y = 1. The result is **u = n + 1, which spans [0,2] for a unit normal**,
sampled with clamp-to-edge. Everything above u = 0.3 reads black, so for almost
every normal on the character this fetch returns 0 and the whole product with it.

The constant identification is checked, not assumed: the shader packs 10 vec4s
and the disassembly uses c0..c6 plus c253/c254/c255, so packed index 7 = c253 =
(2, -1, 0, 0) -- and instruction 5 uses exactly c253.x/c253.y as `r4*2 - 1`, the
standard normal-map decode. That pins the mapping, hence index 8 = c254.

### What is NOT yet established, and it is the whole remaining question

Whether u is wrong because WE compute it wrong, or because the guest handed the
shader a constant/interpolator the console would not have. Both remain open:

  * our texture ADDRESS MODE for this fetch is reported as clamp-to-edge. A
    mirrored mode would fold u = 1.7 back into the white band and light the
    character. `runtime/gpu_draw_textures.cpp:76-85` maps Xenia's ClampMode
    enum, and the two half-border modes (4, 5) are collapsed onto edge/mirror-
    edge -- that collapse is the first thing to check against the real fetch
    constant bits.
  * or r2, the interpolator feeding the normal chain, is wrong -- which would
    ALSO explain the rim term separately measured at ~0 (`saturate(0.3 - N.V)`
    with our N.V ~ 1). One wrong o2 would produce both symptoms, and o2 comes
    out of a 440-instruction vertex shader.

The second is the more economical explanation because it accounts for both
zeros at once. Either way the search is now one fetch and one interpolator wide,
not "the character is black".

### Note (2026-08-06)
## Measured: the texture sign/gamma path moves the character 2.4x, but does not fix it

Following the localisation above, the obvious suspect for a corrupted channel
feeding the env-ramp lookup was the sRGB/gamma decode -- gamma-decoding a NORMAL
MAP is a classic way to wreck exactly the channel this shader remaps
(`r4.w = blue*2 - 1`), and this frame reports 556 of its bindings as kGamma.

Measured on the character region (x 60-240, y 250-520) of bright.gfr:

    default                      max 17   mean 1.81   non-black 46.1%
    GEARS_DRAW_NO_TEX_SIGNS=1    max 29   mean 4.30   non-black 46.5%

So the sign path IS reaching these textures and roughly halves their
contribution -- a real effect, worth knowing. **It is not the cause.** max 29 of
255 is still a black character, and the non-black FRACTION barely moves, which
is what a scale change looks like rather than a restored term.

Recorded so the next session does not spend the hour on it. The localisation
above stands unchanged: the env ramp lookup returns an exact zero because u
lands in the ramp's black two thirds, and the open question is still whether u
is wrong on our side or the interpolator feeding it is.

### Note (2026-08-06)
## The oracle can now be driven along OUR walk: stick input added to its scripted pad

Every wrong conclusion on this entry -- and there have been several today --
traces to the same root: our runtime and the oracle were compared at DIFFERENT
GAME MOMENTS, and there was no way to fix that, because the oracle's scripted
input driver understood BUTTONS ONLY while the runtime's menu walk uses stick
deflections (`GEARS_INPUT_SCRIPT`'s `LY+`, `RX+`). The two sides could not be
given the same walk even in principle.

`tools/xenia_oracle/scripted_input.{h,cc}` now parses stick tokens in the same
notation as its buttons:

    LY+@135     full deflection, held from 135 s onward
    RX-@205     the other direction
    RX0@190     centre that axis

An axis takes the LAST token at or before now and holds it, which is what the
runtime's script does, so one walk can be written for both sides. A stick change
bumps the packet number -- a title that polls a pad whose packet number never
moves treats it as idle however correct the axis values are.

Verified on a 230 s run against the disc, with the runtime's own Act 1 walk
transcribed token for token:

    START@25,A@30,B@35,A@42,A@50,A@60,A@75,A@90,A@105,A@120,
    LY+@135,RX+@160,LY+@175,RX0@190,RX-@205

All four stick events fire at their scripted times (135029, 160023, 190004 and
205000 ms) and the run reaches gameplay. `scratch/oracle/samewalk/frame_0175s.png`
is **Marcus seen from behind, clearly lit, armour detailed**, in the prison
corridor -- the ground truth for difference 1, taken from the same walk our own
runs use.

### What this does NOT yet give, and it should be said plainly

Same WALK is not yet same MOMENT. The two emulations still advance at different
rates, so at 175 s the oracle is looking at a corridor while our runtime at a
comparable frame index is against a different wall. This removes the structural
blocker -- it is now possible to drive both identically -- but closing the
remaining drift needs the frame-indexed stepping `tools/oracle_lockstep.sh`
already has (`f<N>:` on our side, `--oracle_by_frame` on theirs), now that the
stick tokens exist to be stepped.

### Note (2026-08-06)
## Every link in the character's shading chain verified against Xenia -- and it is still black

The localisation above pointed at either our computation of the env-ramp u or
the interpolator feeding it. I have now checked every link between the vertex
buffer and that fetch. All of them match Xenia. Recorded in full so the next
session starts past them rather than through them.

    vertex fetch normalisation   The tangent frame is fetched as FMT_8_8_8_8
                                 NumFormat=INTEGER, and the VS decodes it with
                                 c254.y = 0.007843138 = 2/255 and c255.z = -1,
                                 i.e. raw*(2/255)-1. That only lands in [-1,1]
                                 if the fetch delivers 0..255 rather than
                                 0..1 -- and Xenia's translator skips
                                 normalisation exactly when is_integer is set
                                 (spirv_shader_translator_fetch.cc:297,408).
                                 Measured: 0x00a95110 -> (16,81,169) -> a unit
                                 vector, length 1.0016.
    VS decode constants          c254 = (0.5, 2/255, 2, 0), c255 = (0, 1, -1, 3).
                                 Read out, not assumed.
    interpolator mask            GEARS_DRAW_SPV_DUMP names each module by its
                                 modification: vs ...003f and ps ...0030003f.
                                 Mask 0x3f = all six interpolators exchanged,
                                 so o2 IS passed.
    param_gen                    Our derivation is a verbatim port of Xenia's
                                 IssueDraw, including param_gen_pos, which is
                                 what would shift every interpolator by one if
                                 it were wrong.
    PS constants                 All ten identified and sane: c253 = (2,-1,0,0)
                                 is the normal-map decode, c255.xyz =
                                 (0.11, 0.3, 0.59) are luminance weights,
                                 c3/c4/c5 an orthonormal basis.
    textures                     Normal map decodes to a correct tangent-space
                                 map, diffuse 97.8% non-zero, env ramp real.
    texture signs / gamma        Measured: 2.4x on the character, not the cause.
    clamp mode                   From Xenia's own GetClampModesForDimension.
    draws dropped                Zero, on all four paths, counted.

### The gap, now measured at the SAME WALK

With stick input added to the oracle (see the note above), both sides can run
the runtime's own Act 1 walk. At 175 s the oracle shows Marcus from behind:

    ORACLE character region   max 254   mean 17.5   non-black 100.0%
    OURS                      max  17   mean  1.81  non-black  46.1%

Roughly a factor of ten, and ours is not merely dim -- over half its pixels are
exactly zero. This is not a tonemap difference.

### What is left

Only the vertex shader's own 440-instruction body, i.e. whether our translated
VS computes the same o2 as the console given inputs now shown to be identical.
It is translated by Xenia's own translator, which is why every plumbing check
above passes; so the remaining candidates are narrow and specific:

  * the bone-palette dynamic-addressing path in the SPIR-V backend, which is
    the one construct this shader uses that ordinary geometry does not (it is
    also what `float_dynamic_addressing`, added today for the skinned-frame
    detector, detects);
  * or a guest-side constant outside the palette that this chain reads.

The cheapest next measurement is no longer analytical: with the same walk now
possible on both sides, step them by GUEST FRAME (`oracle_lockstep.sh` has the
machinery, and the stick tokens it lacked now exist) and compare the character
draw's inputs directly rather than reasoning about them.
