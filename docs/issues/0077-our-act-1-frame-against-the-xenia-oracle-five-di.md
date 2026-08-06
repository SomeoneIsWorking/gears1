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
