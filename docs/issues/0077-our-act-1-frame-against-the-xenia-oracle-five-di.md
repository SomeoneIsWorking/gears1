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
