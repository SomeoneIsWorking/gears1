---
id: 62
title: The 3D scene's colour is wrong: red is 78% of green frame-wide, and lit surfaces flatten at 0.30
status: open
symptom: Gameplay looks flat grey-green with blown, detail-free lit surfaces; menus and the title screen look correct in the same run
tags: gpu,draw,colour,tonemap,resolve
created: 2026-08-05
updated: 2026-08-06
---

## The observation that redirects everything

The reported title-screen capture is CORRECT -- dark blue, proper contrast, crisp
text. Only the 3D scenes are wrong, in the same run. That rules out every
whole-frame explanation I pursued for two days: a swapchain format, a colour
space, a compositor transform and an sRGB encode would all wash the menus too.

It is content-specific, which means it is in the part of the pipeline the menus do
not use: the deferred scene path, its HDR surface, the resolve that carries it and
the tonemap that consumes it.

## Measured on our own presented gameplay frame

Whole frame, per channel:  R 0.0772  G 0.0996  B 0.0990
                           -> RED IS 78% OF GREEN, and green and blue are equal.

The sunlit wall through the doorway: mean 0.193, p99 0.298, per-channel
R 0.174 G 0.204 B 0.200. A daylit concrete wall is neutral or warm; this is
uniformly cool, and it flattens at 0.30 with no highlight above it.

Two separate defects in one number: a RED DEFICIT of about a fifth, and a CEILING
that a lit surface cannot pass.

## What this retracts

Every "the renderer's output is correct" in this session was my own judgement of a
dim scene against no reference. It was wrong. The frames I called correct have the
same cast and the same ceiling as the reported screenshots -- the reports were just
of brighter scenes where it is obvious. Catalog #60 and #61 chased the difference
between my captures and the window when the interesting difference was between
BOTH of them and the console.

## Where to look, in order

1. The resolve's `copy_dest_swap` and the exponent bias -- catalog #33 found the
   bias wrong once already (the tonemap's input was 8x too bright), and a red/blue
   asymmetry is exactly what a swizzle or a per-channel scale gets wrong.
2. The 7e3 surface format carried as half-float: an unequal-precision packing would
   hit one channel differently.
3. The tonemap shader's own constants, read from the register file.

An oracle settles it in one comparison and there is one available: Xenia canary
renders this title correctly (user's report), and `extern/xenia` is already
vendored. A single frame of its output next to ours ends the guessing that this
entry exists because of.

### Note (2026-08-05)
## The red deficit is a RED/BLUE SWAP, and it is 100% of pixels

Not a tonemap, not an exponent bias, not a 7e3 packing. The frame we present is
an exact channel exchange of the frame the guest composed.

`GEARS_DRAW_RESOLVE_DUMP=1` now reports **per-channel means** for every resolve
target (new; the old line's range was a max over all three channels, which is
identical whether red is short or not). On courtyard.gfr:

    resolve 0x311000   R 0.076818  G 0.077251  B 0.059357     <- the FRONT BUFFER
    resolve 0xc7f9000  R 0.059357  G 0.077251  B 0.076818
    presented frame    R 0.059357  G 0.077251  B 0.076818

Same three numbers, to six digits, with R and B exchanged. Confirmed per pixel,
not by aggregate:

    presented vs resolve 0xc7f9000 : identical             100.0% of pixels
    presented vs resolve 0x311000  : identical after R/B swap 100.0% of pixels

The capture's `frontBufferAddress` is 0xa0311000 in all three captures, so
0x311000 is the buffer the guest hands to scanout. **What we present is not it.**

R/G = 0.7688 was never a red deficit: red is carrying blue's level. The
"ceiling at 0.30" is a separate observation and is NOT explained by this.

## Which resolve swaps, measured with the existing control arm

`GEARS_DRAW_RESOLVE_NOSWAP=1` changes 0xc7f9000 (R 0.059357 -> 0.076818, i.e.
it matches 0x311000) and leaves 0x311000 untouched. So the guest asks for
`copy_dest_swap` on the resolve to 0xc7f9000 and NOT on the one to the front
buffer, and our resolve honours both correctly. The swap in the presented image
does not come from the resolve.

Both host formats are RGBA -- resolve targets are all R16G16B16A16_SFLOAT
(gpu_draw_xlate.cpp) and the readback is R8G8B8A8 -- so component 0 really is
red in both dumps and the channel labels above are not a BGRA mislabel.

## Next

The presented image is byte-identical to the 0xc7f9000 TEXTURE, which is a
resolve destination, not the surface the front buffer names. So the question is
in the present path, not the colour path: does it present the wrong image, or
does the 0xc7f9000 resolve write into the host image that gets presented?
Read the presentBase selection in gpu_draw.cpp (~line 1730) and what the
readback at ~1866 actually copies from.

Do not re-open the tonemap, the exponent bias or the 7e3 packing on this
evidence: a per-pixel 100% match cannot be produced by any of them.

### Note (2026-08-05)
## RETRACTED: the note above is WRONG where it concludes a swap DEFECT

The measurements in it are right. The inference from them is not, and the
entry must not be read as "the present path swaps channels".

**What is still true.** The presented frame is byte-identical to resolve target
0xc7f9000 and an exact R/B exchange of resolve target 0x311000, per pixel,
100.0% both ways. Those numbers stand.

**What is false.** That this shows a defect, and that it explains R/G = 0.7688.

Three things refute it, each measured after the fact:

1. **Both resolves ask for the swap.** The diag table now carries
   `resolve_swap_rb` (new column): draw 717 -> 0xc7f9000 and draw 743 ->
   0x311000 BOTH have swap_rb = 1. The earlier reading -- "the guest asks for
   the swap on one and not the other" -- came from the NOSWAP control arm
   moving only one of the two, and that is not what it means.
2. **Nothing fell back to the blit.** `resolvesUnstorable` is 0 on this frame,
   so no resolve silently dropped the swap; every one ran the compute path.
3. **The guest exchanges the channels itself, mid-frame.**
   `GEARS_DRAW_PIXEL_TRACE=835,258` on consecutive draws:

       after 527 draws (surface 0x2d0) = (0.296875, 0.296875, 0.2685547, 0) <- draw 702
       after 528 draws (surface 0x2d0) = (0.2685547, 0.296875, 0.296875, 0) <- draw 703

   Same pixel, same surface, R and B exchanged by ONE of the title's own draws.

That closes the loop consistently: 0xc7f9000 resolves the surface before draw
703, 0x311000 resolves it after, both applying the swap, and the presented
surface is the later state. Every relationship I measured follows from the
guest's own stream, and none of it requires a defect. It also agrees with the
present path's own reasoning in gpu_draw.cpp (~line 1797), which says
presenting the source surface rather than the front buffer is deliberate and
was settled against the boot movie -- I should have read that BEFORE concluding.

## So where #62 stands

Unchanged from before that note. R/G = 0.7688 and the ~0.30 ceiling both still
reproduce on today's build (courtyard.gfr: R 0.0594 G 0.0773 B 0.0768, p99
0.29), and the cause is NOT established. **The tonemap, the exponent bias and
the 7e3 packing are NOT ruled out** -- the previous note said they were, and
that exclusion rested on the same bad inference.

The oracle comparison the original entry asked for is still the thing that
would settle it, and nothing since has replaced it.

## What the detour did leave behind

Two instruments that stay useful and are validated:
- `GEARS_DRAW_RESOLVE_DUMP=1` reports per-channel means and R/G, B/G per
  target. The old line's range was a max across all three channels and reads
  identically whether one channel is short.
- The diag table has `resolve_swap_rb` and `resolve_scale`, so what a resolve
  was ASKED to do is readable without toggling a knob and diffing images.

### Note (2026-08-05)
## The oracle: what is ready, and what needs a person

The entry has said since it opened that an oracle settles this in one
comparison. Scoped this session; the blocker is not the build.

READY, nothing to do:
- Xenia is built on this machine and launches: `scratch/oracle/xenia-canary/
  build/bin/Linux/xenia_canary` (19 MB, catalog #7 has the three local
  workarounds and the exact configure line).
- The title's data is extracted at `scratch/game` (`default.xex` present), so
  it can be loaded directly -- no disc image step.
- `tools/frame_stats.py` (new) reports our frame and a reference frame with the
  SAME statistics from one command, PPM or PNG, so the comparison is not two
  ad-hoc scripts with two definitions. `--selftest` proves it reports both
  answers. It reproduces this entry's numbers on courtyard.gfr:
  R/G 0.7684, and a p99.9 of 0.298 against a max of 1.0 -- the 0.30 ceiling is
  a real ceiling with a handful of pixels above it, not a soft rolloff.

NEEDS A PERSON: reaching a gameplay scene in Xenia takes a window and a
controller, and that is an interactive session rather than something a
measurement run can do. Note `xenia_canary --help` does NOT print help and
hangs (it went to a UI picker); kill it by PID.

So the remaining step is one short interactive Xenia session that reaches any
Gears gameplay scene and takes a screenshot. It does NOT have to match one of
our captures: the question is whether a correct renderer of this title also
puts red a fifth below green frame-wide with a ~0.30 ceiling. If it does, the
premise of this entry is wrong and the frames were always right. If it does
not, the entry's three suspects are back on with a reference to aim at.

Then: `tools/frame_stats.py --diff <ours.ppm> <xenia.png>`.

### Note (2026-08-05)
## The oracle plan, corrected: build a harness, do not drive the emulator

The earlier note here said the remaining step was "one short interactive Xenia
session". That was wrong about what is needed. Xenia ships a HEADLESS renderer
for exactly this -- `xenia-gpu-vulkan-trace-dump`, which loads a GPU trace,
renders it and writes the frame with no window and no controller. It now builds
(catalog #7 has the one-line recipe and the artifact path).

So the shape is: OUR capture -> a Xenia GPU trace -> their renderer -> a PNG ->
`tools/frame_stats.py --diff`.

## The caveat that decides how much the answer is worth

A `.gfr` stores per-draw REGISTER SNAPSHOTS, decoded microcode and guest pages.
A Xenia trace stores a PM4 PACKET STREAM that their command processor executes.
We do not keep the packets, so a converter has to SYNTHESISE a command stream
from our snapshots -- which means encoding our own reading of the registers into
the input.

That is fine for what #62 asks (does a correct renderer of these draws put red a
fifth below green?) because the question is about SHADING, RESOLVE and FORMAT,
downstream of the decode. It is worthless for any question about command-stream
decoding: on those the oracle would agree with us by construction, exactly where
we are wrong. Say which kind of question is being asked before trusting a run.

The alternative with no such caveat is a trace RECORDED from Xenia running the
title, which needs the game played once. Strictly better evidence, higher cost.
Do not conflate the two arms or report one as if it were the other.

### Note (2026-08-05)
## The .gfr -> .xtr converter exists, and Xenia executes our draws

`tools/gfr_to_xtr.py` (new) turns a capture into a Xenia trace. Xenia's headless
renderer now parses it, loads the frame's 182 MiB of guest pages, restores each
draw's registers and EXECUTES our synthesised DRAW_INDX packets. It does not yet
produce an image; what is left is one specific, identified thing.

Working, verified by Xenia's own log rather than by the file being written:
- 744 draws converted from courtyard.gfr into a 240 MiB trace.
- `kMemoryRead` for guest pages -- NOT `kMemoryWrite`, which trace_player.cc
  ignores on playback; that choice would have rendered from empty memory.
- `kRegisters` per draw, clamped to Xenia's `kRegisterCount` (0x5003). We
  capture 0x8000, and RestoreRegisters rejects an overrun with a warning and
  then renders from an unset register file -- which presents as a backend
  failure, not a format mismatch. That warning is gone.
- Packets big-endian in guest memory (Xenia reads them with ReadAndSwap) while
  trace command headers stay little-endian.
- The packet scratch page is CHOSEN per capture from pages the capture leaves
  empty. The first hardcoded guess (0x01000000) collided with real data in
  courtyard.gfr and the tool refused rather than corrupting the frame.

## What is left: the shaders are never bound

Every draw fails with `PM4_DRAW_INDX(...): Failed in backend`, and the reason is
in pm4_command_processor_implement.h:1309 -- `active_vertex_shader_` and
`active_pixel_shader_` are set ONLY by `PM4_IM_LOAD`. No register assignment
sets them, so restoring the register file cannot bind a shader and every draw
reaches the backend with none.

The fix is to emit an IM_LOAD per shader before each draw:

    addr_type  = <guest address of the microcode> | shader_type   (0x3 mask)
    start_size = (start << 16) | size_dwords                      (start == 0)

The capture stores the microcode BLOBS and each draw's blob indices, but NOT the
guest address the microcode lived at -- so the converter must place each blob in
a free guest page itself (the same free-page search the packet scratch already
uses) and IM_LOAD from there. The .gfr parser currently skips over the blob
bytes; it needs to keep them.

Until that lands this produces no reference image, and nothing here should be
quoted as an oracle result.

### Note (2026-08-05)
## Shaders bind correctly; the failure is further into IssueDraw

The converter now emits `PM4_IM_LOAD_IMMEDIATE` (0x2B) per shader before each
draw, with the microcode embedded in the packet. Two things this pins down:

**The packet stream is read correctly.** Exactly 744 "Failed in backend"
messages for exactly 744 draws -- no extra packets, none dropped. And the
message decodes as (num_indices, prim_type, source_select), so
`PM4_DRAW_INDX(3, 8, 2)` is draw 0, which the capture independently says is
prim 8, 3 indices, auto-index. The synthesised packets are being parsed as
intended.

**The shaders ARE bound.** I added an XELOGE to IssueDraw's first early-out
(`!vertex_shader`, previously a silent `return false`) on our fork, rebuilt,
and it does NOT fire. So `IM_LOAD_IMMEDIATE` works and the draw dies later.
Recording this as a NEGATIVE worth keeping: without it the next session would
re-suspect shader binding, which is the obvious suspect and is wrong.

Encoding notes, since they cost time: the count field covers the two body
dwords PLUS the inline microcode, and the microcode must NOT be byte-swapped --
IM_LOAD_IMMEDIATE reads it through read_ptr() directly while the packet's own
dwords go through ReadAndSwap. A shader too large for the 14-bit count is
refused rather than truncated (a truncated shader translates to something
plausible and wrong).

## Next: name the remaining early-outs

`IssueDraw` has about a dozen silent `return false`s and they all surface as one
message. The survivors, in order, are: BeginSubmission,
primitive_processor_->Process, host_draw_vertex_count == 0,
EnsureShadersTranslated, render_target_cache_->Update, ConfigurePipeline.
Do what was done for the first one -- give each an XELOGE on the fork and rerun
`--draws 1`. Do NOT guess between them; one rebuild answers it.

Draw 0 being prim 8 with 3 vertices (a rectangle list -- UE3's full-screen
clear) makes `host_draw_vertex_count == 0` a reasonable first suspect, but that
is a hypothesis and the log will say.

### Note (2026-08-06)
## The oracle comparison this entry asked for, and the ceiling is EXACT (2026-08-06)

"An oracle settles it in one comparison and there is one available" -- done, with
a trusted headless oracle (I014/C013, `tools/xenia_oracle`, disc image mounted).
Same scripted walk, `tools/frame_stats.py` on both sides:

    ORACLE frame_0210s.png   R p99 0.808  G p99 0.784  B p99 0.722   p99.9 1.000
    OURS   frame_06300.ppm   R p99 0.267  G p99 0.298  B p99 0.298   p99.9 SAME

The reference reaches 1.0. We stop at 0.30, and **p99.9 equals p99 to three
decimals**, which a tonemap roll-off does not do -- that is a hard clamp.

## The ceiling is the SAME THREE BYTES in every gameplay frame

Maxima of our own presented frames across one scripted walk, per channel, of 255:

    frame_00900   R 255  G 255  B 255      <- menu, no ceiling
    frame_01800   R  68  G  76  B  76
    frame_02700   R 196  G 197  B 196      <- transition
    frame_03600   R  68  G  76  B  76
    frame_04500   R  68  G  76  B  76
    frame_05400   R  68  G  76  B  76
    frame_06300   R  68  G  76  B  76

Five different game moments, byte-identical maxima. No pixel above 200 in any of
them. That is not scene content and not a roll-off: it is a constant, and
(68, 76, 76) is a far sharper thing to chase than "flattens at 0.30".

Note the ceiling is per-channel UNEQUAL -- 68 against 76 -- in the same ratio
(0.895) as the R/G deficit this entry already tracks. Whatever sets the ceiling
and whatever sets the channel ratio may be the same thing, which the retracted
red/blue-swap note could not have shown either way.

MENUS DO NOT CLAMP (frame_00900 reaches 255), which agrees with this entry's
founding observation and keeps the search in the deferred scene path.

## Not re-derived

The red/blue relationship is settled and is the GUEST's own mid-frame exchange
(see the retraction above and claim C011). Nothing here reopens it. The ceiling
was always the separate observation, and it is still the open one.

### Note (2026-08-06)
## CORRECTION to the note above: it is NOT a ceiling, it is a dark image

The maxima are right; "clamp" is the wrong word for them, and it points at the
wrong mechanism.

Those frames use EVERY level up to their maximum -- 69 distinct values in red
(0..68) and 77 in green and blue (0..76). A clamp piles pixels up AT the
ceiling; this has no pile and nothing above. The image is not a wide range with
its top cut off. It is a narrow range: the composite never produces a bright
pixel at all, and then fills the bottom third of the byte smoothly.

So the search is for a MISSING SCALE (255/76 = 3.36x), or a scene that arrives
at the composite too dim -- not for something that truncates.

That also reconciles it with what ef39f34 measured and I nearly contradicted:
distinct levels per channel 69/77/77 without the ramp. Same numbers, because
"levels" and "max+1" are the same thing for a contiguous range. Nothing here is
a new symptom; the framing was wrong.

## These frames are PRE-RAMP, checked rather than assumed

scratch/oracle/compare/ours dates from 2026-08-05 23:02; the gamma ramp landed
in 6be02aa at 2026-08-06 00:09. So (68, 76, 76) is the composite's own output
with no scan-out LUT involved, and the ramp is not a candidate for it. The
oracle frames compared against them are from the SAME oracle_compare.sh run, so
the two sides are the same walk rather than two unrelated sessions.

## Still unexplained, and now more sharply

ef39f34 measured that at draw 480 the linear-light scene buffer has the windows
SATURATED to pure white. So the scene buffer is too BRIGHT at the top while the
composite's output is too DARK everywhere. Whatever sits between them compresses
rather than clips, and that pair -- saturated input, 0.3-maximum output -- is
the shape of the defect to explain.

### Note (2026-08-06)
## An OFFLINE repro, and two candidates killed (2026-08-06)

### act1.gfr reproduces it in 550 ms

Replayed every capture and measured the presented maximum per channel:

    act1       max R102 G106 B106   levels 103/107/107   CONTIGUOUS  <-- reproduces
    act1_now   max R255 G255 B255   levels 243/245/247   gappy
    act1_v2    max R255 G255 B255   levels 246/242/253   gappy
    black      max R196 G197 B196   levels 150/157/154   gappy
    courtyard  max R255 G250 B255   levels 144/142/169   gappy

Only `act1` has the narrow-contiguous signature of the walk frames (68/76/76
there, 102/106/106 here -- same shape, different scene). So this no longer costs
a 200 s scripted walk: `frame_replay scratch/frames/act1.gfr`.

Note the channel asymmetry is NOT a fixed constant -- R/G is 0.895 in the walk
frames and 0.962 here -- so whatever makes red short is scene-dependent and is
not a single hardcoded scale.

### KILLED: the HDR scene surface being clamped by its host format

Surface 0x400 (k_2_10_10_10_FLOAT, the scene) gets VK_FORMAT_R16G16B16A16_SFLOAT
and its content exceeds 1.0. The per-draw clamp our shaders apply is
`kAlphaOnly` for that format, which is correct -- RGB unclamped.

### KILLED, after I nearly reported it as the smoking gun

The scene resolve destination 0xbde0000 ends the frame with a maximum of
EXACTLY 0.125000 = 2^-3, which reads as "a source clamped at exactly 1.0, times
the resolve's own exp_bias of -3".

It is not. The resolve LIST says the last write into 0xbde0000 is draw 670 from
surface **0x2d0**, not from the scene surface 0x400 -- and 0x2d0 is used with
k_8_8_8_8, whose guest clamp to [0,1] the hardware performs too. So a maximum of
exactly 1.0 there is FAITHFUL, and 0.125 after the bias is the correct answer.
The dump reports end-of-frame state; four resolves land in that target with two
different formats and two different biases, and reading the last one as if it
came from the scene pass is what made it look like a defect.

### INSTRUMENT CAVEAT: GEARS_DRAW_RESOLVE_SCALE cannot read a source's range

Forcing scale 1.0 to "see the unbiased source" does not work on this frame, and
the number it gives is not what it looks like. The composite SAMPLES resolve
targets, so making every resolve 8x brighter changes what the later passes
compute and therefore changes the surfaces the later resolves read. The two arms
are not the same frame. It reported the source at 1.504883; that value describes
a perturbed chain, not this one.

To read a surface's own range, use something that does not alter it --
GEARS_DRAW_PIXEL_TRACE reads in the surface's native format without changing the
pass.

### Checked and fine: the texture fetch exponent

`exp_adjust` (fetch constant word 3, bits 13:18) is what would compensate a
resolve's exp_bias on the sampling side, and its absence would make everything
that samples a biased resolve 8x too dark. Xenia's translator reads it from the
fetch-constant uniform block at runtime, and `gpu_draw_uniforms.cpp` uploads all
6 dwords x 32 slots verbatim from register 0x4800, so the shader has the real
value. Not a candidate.

### Note (2026-08-06)
Bloom is a concrete candidate for the missing top of the range: the whole bloom chain renders black (catalog #81) -- surface 0x5a0 has 0 non-black pixels after all five of its draws and its resolve target is 0 of 192192 components non-zero, while the draws themselves shade 57600 fragments each with verdict 'shaded'. The final tonemap therefore composites with no bloom term at all. Not yet established that fixing it fixes this entry.

### Note (2026-08-06)
The bloom path is now explained and is a SYMPTOM of this entry, not a separate defect (catalog #81): the bright pass thresholds every sample with sgt against c255.x = 1.0, and its input resolve target tops out at 0.125, so it writes zero everywhere. That gives this entry a quantitative target for the first time -- for bloom to do anything, surface 0x2d0 must exceed 8.0, because the resolve feeding the bright pass applies copy_dest_exp_bias -3. Ours maxes at exactly 1.0.

### Note (2026-08-06)
## TWO INDEPENDENT ROUTES NOW AGREE ON A NUMBER: the scene is ~3.5x too dim

Built the missing probe (`GEARS_DRAW_SURFACE_RANGE=1`, knobs.md) and pointed it
at act1. Per surface, at end of frame:

    surface 0x0    R 0.0000..0.0000  G 0.0000..0.0000  B 0.0000..0.0000
    surface 0x2d0  R 0.0000..0.3992  G 0.0000..0.4172  B 0.0000..0.4146   0 px > 1.0
    surface 0x400  R -0.0001..1.9531 G 0.0000..2.1914  B 0.0000..2.0957   1715 px > 1.0 (0.19%)
    surface 0x5a0  R 0.0000..0.0000  G 0.0000..0.0000  B 0.0000..0.0000

Three things fall out, two of them corrections.

**The HDR scene is NOT clamped.** Surface 0x400 reaches 2.19 with 1715 pixels
above 1.0. Every "something clamps the scene at 1.0" hypothesis is dead,
including the one this entry's earlier note inferred from a resolve maximum of
exactly 0.125.

**CORRECTION to that inference.** I read "resolve destination maxes at exactly
0.125 = 1.0 x 2^-3" as "surface 0x2d0's max is exactly 1.0". Measured directly,
0x2d0 ends the frame at 0.417, not 1.0. Both can be true -- the resolve happened
at draw 670 and the surface kept changing afterwards -- but the inference was
not sound, and a surface range is now measurable instead of reconstructed.

**And the two routes converge.** Independently:

  * the presented frame tops out around 0.30 where the oracle reaches 1.0
    -- a shortfall of about 3.4x;
  * bloom's threshold is 1.0 against a resolve biased by 2^-3, so the source
    must reach 8.0 for the effect to exist at all, and our scene peaks at 2.19
    -- a shortfall of about 3.7x.

Two measurements taken from different ends of the pipeline, agreeing on ~3.5x.
That is the shape of a missing SCALE in the scene's absolute brightness --
lighting or exposure -- and not of a clamp, a resolve, a format or bloom, all of
which are now measured and behaving.

NOT established: where the factor is applied or omitted. What is established is
its size and that it is upstream of everything this entry has ruled out.

### Note (2026-08-06)
## RETRACTED: "two routes agree the scene is ~3.5x too dim" (2026-08-06)

That note was built entirely on act1, and running the same probe across every
capture refutes it. The scene surface, per capture:

    capture      surface 0x400 max (R/G/B)        px above 1.0
    act1         1.95 / 2.19 / 2.10               1715  (0.19%)
    black        34.16 / 31.78 / 17.19           12135  (1.32%)
    courtyard    37.09 / 30.98 / 16.34           19161  (2.08%)
    act1_now     0 / 0 / 0                            0
    act1_v2      0 / 0 / 0                            0

**Courtyard's scene reaches 37.** That is the full 7e3 range the format carries,
so the scene pass produces proper HDR and there is no global scale missing.
act1's 2.19 is act1 being a darker moment, not a defect, and the "~3.7x
shortfall against bloom's threshold" I derived from it measured the scene rather
than the renderer.

I should have run the probe against a second capture before writing the
convergence down. One number from one frame, agreeing with another number from
the same frame, is not two independent routes.

## What survives, and the sharper question it leaves

Still true and still this entry's core: our presented GAMEPLAY frames top out at
0.30-0.42 while the oracle reaches 1.0, and five walk frames share the maxima
68/76/76 exactly.

But it is now clear that this is MOMENT-SPECIFIC rather than global. Presented
maxima by capture:

    act1        102 / 106 / 106   contiguous   <- narrow, like the walk frames
    courtyard   255 / 250 / 255   gappy        <- full range
    act1_v2     255 / 255 / 255   gappy
    black       196 / 197 / 196   gappy

So the renderer produces a full-range image on some captures and a narrow one on
others, and courtyard does it with a scene surface reaching 37 while act1 does
not with one reaching 2.19.

THE QUESTION IS NOW: what distinguishes the frames that present a full range
from the frames that stop at 0.30-0.42? Both classes are gameplay, both go
through the same passes. Comparing act1 against courtyard pass by pass is a
comparison of two captures the frame_replay harness can run back to back, which
is the cheapest form this investigation has ever had.

### Note (2026-08-06)
## THE WALK, BOTH SIDES, EVERY FRAME -- this is real and it is consistent (2026-08-06)

The previous notes compared one frame against one frame, which is why the
"3.5x" reading did not survive. Comparing every frame of the same scripted walk,
green channel, from `scratch/oracle/compare`:

    OURS                                        ORACLE
    frame_00900  max 255  p99 121  p99.9 196    0030s  max 255  p99  48  p99.9 189
    frame_01800  max  76  p99  67  p99.9  75    0060s  max 254  p99  23  p99.9  75
    frame_02700  max 197  p99  16  p99.9 189    0090s  max 146  p99   9  p99.9  37
    frame_03600  max  76  p99  75  p99.9  75    0120s  max 255  p99 137  p99.9 197
    frame_04500  max  76  p99  75  p99.9  75    0150s  max 255  p99 198  p99.9 254
    frame_05400  max  76  p99  75  p99.9  75    0180s  max 255  p99 185  p99.9 254
    frame_06300  max  76  p99  75  p99.9  75    0210s  max 255  p99 199  p99.9 254
                                                0240s  max 255  p99 178  p99.9 254

FIVE CONSECUTIVE GAMEPLAY FRAMES OF OURS TOP OUT AT 76, with p99.9 at 75 -- so
essentially every pixel is below 76 and there is nothing in the top 70% of the
byte. Every gameplay frame on the oracle's side reaches 255 with p99 between 137
and 199.

This is not moment-specific and it is not one unlucky pair. It is every
gameplay frame on one side against every gameplay frame on the other, from the
same scripted walk.

And it agrees with this entry's founding observation rather than contradicting
it: our EARLY frames (00900 at max 255, 02700 at 197) are the menu and the
loading screens, and they behave like the oracle's early frames. The defect is
specific to the deferred gameplay path.

So the retraction above stands (there is no global 3.5x scale, and act1's dark
scene was not evidence of one), and the entry's core is now on much stronger
footing than it has ever been: a whole-walk comparison against a trusted oracle
rather than a single frame pair.

## The final tonemap's inputs, and one thing NOT to chase

`GEARS_DRAW_TEX_BINDS=629226076307234e` -- the last full-screen pass before the
front-buffer resolve, and the same shader in both captures:

    fc0 -> 0xc7e9000  (the composite)   exp_adjust +0
    fc1 -> 0xba40000  (depth)           exp_adjust +0
    fc2 -> 0xcb81000  ALL ZERO          exp_adjust +0

fc2 is entirely black -- it resolves surface 0x2d0 immediately after a clear.
That looks like a lead and is NOT one: courtyard's equivalent target
(0xcb91000) is equally all-zero, and courtyard presents a FULL 255 range. A
black fc2 is therefore common to both classes and cannot be what separates them.
Recorded so the next session does not spend an hour on it.

### Note (2026-08-06)
## Checked: the narrow frames really are GAMEPLAY (2026-08-06)

The whole-walk comparison above rests on the capped frames being gameplay rather
than loading screens, and that was assumed rather than checked. From the same
run's log, draws issued per reported frame, in order:

    frame_00900   162 of 177 draws     max 255    <- menu
    frame_01800   639 of 849 draws     max  76    <- gameplay
    frame_02700   365 of 451 draws     max 197    <- transition
    frame_03600   497 of 677 draws     max  76    <- gameplay
    frame_04500   502 of 684 draws     max  76    <- gameplay
    frame_05400   537 of 723 draws     max  76    <- gameplay
    frame_06300   514 of 694 draws     max  76    <- gameplay

677-849 guest draws is the deferred UE3 pipeline; 177 is the title screen. So
the frames that cap at 76 are the gameplay ones and the frame that reaches 255
is the menu, which is what this entry has claimed since it was opened -- now
with the draw counts behind it rather than an inference from brightness.

### Note (2026-08-06)
## RETRACTED AGAIN: there are not two classes. MAX is a one-pixel statistic.

I split the captures into "narrow" and "full range" by their presented MAXIMUM,
and used that split to retract the scale hypothesis and to propose comparing
act1 against courtyard pass by pass. The split does not exist.

Captured a gameplay frame from the walk itself (744 guest draws,
`scratch/frames/walk_gameplay.gfr`, replays in ~1 s) and compared its
distribution against courtyard's, green channel:

    walk_gameplay (replayed)   median 12   p99 74   p99.9 75   max  84   px>128: 0
    courtyard     (replayed)   median 12   p99 74   p99.9 75   max 250   px>128: 26

**Identical to the percentile.** Courtyard is not a full-range frame: it is the
same narrow image with TWENTY-SIX pixels above 128 out of 921,600 -- 0.003%, a
specular highlight or a lamp. Reading its max as "full range" was reading one
pixel.

So every gameplay capture we have is in one class, and the earlier note's
"the renderer produces a full-range image on some captures and a narrow one on
others" is withdrawn, along with the act1-versus-courtyard comparison it
proposed. act1 is simply a darker moment within the same one class.

## What the whole thing actually says now

    ours    (5 gameplay frames)  p99  74-75   p99.9  75     max 76
    oracle  (5 gameplay frames)  p99 137-199  p99.9 197-254  max 255

and on the SAME defect-class frame, measured directly:

    surface 0x400 (the HDR scene)  max 37.09 / 30.98 / 16.34
                                   19161 px above 1.0  (2.08%)
    surface 0x2d0 (the composite)  max  0.27 /  0.33 /  0.30
                                   0 px above 1.0
    presented                      max 68 / 84 / 76

The scene surface carries proper HDR -- 37, with 2% of the frame above 1.0 --
and the composite surface it becomes never exceeds 0.33. **The range is lost
between the scene surface and the composite**, in a frame that is measured
rather than inferred, with both ends of the loss on the same run.

That is the narrowest the search has been, and it does not depend on any
comparison between two different captures.

## The lesson worth keeping

Three retractions in this investigation, and this one and the last both came
from a summary statistic that could not see what I was asking it. Max cannot
distinguish "a bright image" from "a dark image with one lamp in it". Every
brightness claim here should quote a PERCENTILE, and `tools/frame_stats.py`
already reports p99 and p99.9 -- I stopped using it and went back to max.

### Note (2026-08-06)
## LOCALISED TO ONE DRAW BOUNDARY: RGB clamped to exactly 1.0, alpha untouched

On `scratch/frames/walk_gameplay.gfr` (defect class, 744 draws), the surface
probe now reports WHERE its maximum is, so a pixel trace can be aimed instead of
guessed:

    surface 0x400 brightest pixel is (368,247) at 37.0938

Following that pixel on surface 0x2d0 (`GEARS_DRAW_SURFACE=0x2d0
GEARS_DRAW_PIXEL_TRACE=368,247`), in the surface's own format:

    after   2 draws  (0, 0, 0, 0)                          <- draw   1
    after 437 draws  (37.09375, 30.984375, 15.90625, 0.125) <- draw 612
    after 468 draws  (1, 1, 1, 0.125)                       <- draw 643
    after 527 draws  (0.29711914, 0.29711914, 0.2685547, 0) <- draw 702
    after 528 draws  (0.2685547, 0.29711914, 0.29711914, 0) <- draw 703

**The full HDR scene value reaches surface 0x2d0 intact** -- 37.09, the same
number the scene surface holds. And between the samples either side of draw 643
it becomes exactly (1, 1, 1) with **alpha preserved at 0.125**.

That signature is the useful part. It is not a shader writing white (alpha would
change), not a clear (alpha would change), and not a tonemap (it would not land
on exactly 1.0 in all three channels). It is an RGB-only clamp to [0,1].

## And draw 643 cannot be the one writing it

    draw 643  surface 0x2d0  color_fmt 0  color_mask 0  verdict colour_fully_masked
              triangle_list, 4248 indices, 2278 fragment invocations, blend off
              viewport and scissor 422x422

`color_mask 0` means no colour channel is written, and the pipeline HONOURS it
(`gpu_draw_pipelines.cpp` maps the mask bits to `colorWriteMask`, so zero writes
nothing). The column is real, not a stuck value: across the frame it is 0 on 314
draws, 15 on 237 and 7 on one, and exactly three draws are classified
`colour_fully_masked` -- 612, 643 and 644, which are the three the trace names.

So the clamp is NOT draw 643's colour output. Something at that boundary rewrote
RGB and left alpha alone.

## What to look at next, in order

  1. The render-pass SPLIT. This renderer ends and resumes the pass around draws
     that sample the rendered RT, copying colour into a separate image
     (`copyColorToImage`, `renderPassLoad`). A copy through an 8888 view would
     clamp RGB and could leave alpha, and the split points are exactly draw
     boundaries.
  2. Surface 0x2d0 is the WIDENED one -- five guest formats in one float16 host
     image -- and draw 643's guest format is k_8_8_8_8 while draw 612's is
     k_2_10_10_10_FLOAT. A reinterpretation between those two is the frontier's
     long-listed and never-tested candidate (draw-backend-rt, gap 1).
  3. Our own per-draw guest clamp (`GuestColorFormatClamp`) produces exactly
     this shape for a k_8_8_8_8 target on a widened surface -- but it is applied
     in the pixel shader, and a masked draw's shader output goes nowhere, so it
     should not be reachable here. Worth confirming rather than assuming.

### Note (2026-08-06)
## The window narrowed to two draws, and a correction to my own signature reading

### CORRECTION: "alpha preserved, so it is an RGB-only clamp" was wrong

The traced pixel goes (37.09, 30.98, 15.91, 0.125) -> (1, 1, 1, 0.125), and I
read the unchanged alpha as meaning only RGB was touched. **Alpha 0.125 is
already inside [0,1]**, so an ordinary four-channel clamp to [0,1] leaves it
exactly there. The signature is a plain clamp, which WIDENS the candidate list
rather than narrowing it, and the note above claimed the opposite.

### Where it happens, to two draws

The trace prints only the samples that CHANGED, so the value was still 37.09 at
the sample after draw 640 and was (1,1,1) at the sample after 643. The frame in
between:

    640  surf 0x2d0  fmt  0  mask 15  rectangle_list  0 frags   rasterised_no_fragment
    641  surf 0x0    fmt  0  mask  0  rectangle_list  0 frags   rasterised_no_fragment
    642  surf 0x0    fmt  0  mask  0  rectangle_list  0 frags   rasterised_no_fragment
    643  surf 0x2d0  fmt  0  mask  0  triangle_list   2278      colour_fully_masked

Draw 643 writes no colour (mask 0, and `gpu_draw.cpp:1098` and the pipeline both
read RB_COLOR_MASK from R[0x2104], so the diag and the pipeline agree). Draws
641 and 642 target a DIFFERENT surface, 0x0, whose host image is
R8G8B8A8_UNORM -- an 8-bit format that cannot hold 37.09.

So the window contains exactly one interesting event: a switch away from surface
0x2d0 to an 8-bit surface and back, with the render-pass end/begin that implies.

### Two control arms that CANNOT answer this -- do not retry them

Both were tried and both are too destructive, because they remove the composite's
input rather than isolating the clamp:

  * `GEARS_DRAW_NORT=1` -- the pixel never leaves (0,0,0,0). The composite reads
    a resolve target, and without the RT link it reads a stub.
  * `GEARS_DRAW_ONLY_BASE=0x2d0` -- same, for the same reason: the scene arrives
    on 0x2d0 THROUGH a resolve of another surface.

A useful arm has to keep the input and change only the suspected mechanism.

### Also checked and NOT the cause

The one draw in the frame with colour mask 7 (RGB, no alpha) is draw 718, the
final tonemap, 75 draws later. It matched the signature I had misread and is not
in the window.

The `vkCmdClearColorImage` that fills with (1,1,1,1) is the white STUB texture
for unbound samplers, not a surface -- and it would set alpha to 1.0, which did
not happen.

### Note (2026-08-06)
## The same pixel on both surfaces: RGB identical, ALPHA divided by exactly 8

Traced (368,247) on the scene surface as well as the composite, same frame, same
run:

    surface 0x400 (scene)     after draw 347: (37.09375, 30.984375, 15.90625, 1)
    surface 0x2d0 (composite) after draw 612: (37.09375, 30.984375, 15.90625, 0.125)

**RGB is bit-identical and alpha is exactly one eighth.** 0.125 is 2^-3, and -3
is precisely the `copy_dest_exp_bias` every colour resolve in this frame carries.

So on the path from the scene surface to the composite surface, an exponent bias
of 2^-3 was applied to ALPHA and not to RGB. The resolve shader multiplies a
float4 by the scale (`gpu_draw_xlate.cpp`: `c *= scale`, all four components,
matching Xenia's `pixel_0 *= exp_bias`), so a result with three components
unscaled and one scaled is not what that code should produce.

This is the sharpest single anomaly this entry has, and it is one pixel measured
twice rather than an aggregate.

WHAT IT DOES NOT YET SAY: which stage did it. The composite draw 612 has colour
mask 0 and cannot write, the resolve runs 200 draws earlier, and the value on
0x2d0 could have arrived by either. It is also not established that this
asymmetry causes the clamp at draw 643 -- they may be two faces of the same
mishandling or unrelated.

## The pixel trace is VALIDATED, not merely used

Both surfaces were traced with the same instrument, and on the scene surface it
reports a history that makes sense on its own terms:

    after 259 draws  (0, 0, 0, 0)                            <- draw 258
    after 263 draws  (2.9785156, 3.3339844, 3.3496094, 1)    <- draw 262
    after 348 draws  (37.09375, 30.984375, 15.90625, 1)      <- draw 347

A cleared pixel, then a base-pass draw putting light into it, then a brighter
one. Real shaded draws, ascending values, alpha 1. That is what made the
0x2d0 alpha of 0.125 stand out instead of being read past -- and it is the check
that keeps "the value changed at a colour-masked draw" from being read as an
instrument fault, which is what I suspected before running it.

### Note (2026-08-06)
## Four mechanisms eliminated, and the trace now attributes exactly

The finding to explain: with the pixel trace pinned to surface 0x2d0, the value
changes at draws 612 and 643, and BOTH have RB_COLOR_MASK = 0.

### 1. The trace was mis-attributing -- FIXED, and the answer did not move

`GEARS_DRAW_SURFACE` dropped every sample taken while another surface was bound,
so a change happening across a switch landed on the next sampled draw. Draw 611
targets surface 0x0, so 612 was the first sample after it -- exactly the shape of
a mis-attribution.

Fixed: with a filter set, the trace now samples that surface after EVERY draw,
whatever is bound. Samples went from 364 to 550 and **the rows are unchanged** --
still 612 and 643. So the attribution is exact and the escape hatch is gone.

### 2. R[0x2104] really is RB_COLOR_MASK

Checked against Xenia's own register table
(`register_table.inc:1265: XE_GPU_REGISTER(0x2104, kDword, RB_COLOR_MASK)`), so
mask 0 on those draws is not a wrong-register artifact.

### 3. The `openSurface = 0` sentinel does NOT collide with base 0x0

Surface base 0x0 is a real surface in this frame AND 0 is the initial value of
the "no pass open" tracking, which looked like a classic sentinel bug. It is
guarded: the switch test is `!inPass || openSurface != pd.surfaceBase`, so with
no pass open a pass is opened regardless of base.

### 4. The pipeline cache key includes the colour mask

A masked draw reusing an unmasked pipeline would explain everything. It cannot
happen: the key is `(vsMod, psMod, gsMod, primType, om, renderPass)` and
`OutputMergerState::operator<` compares `colorMask` first
(`gpu_draw_formats.h:103`).

### What is left

The only event still bound to exactly those draws is the RENDER PASS BEGIN. Both
612 and 643 are the first draw on 0x2d0 after a draw on another surface, so each
one ends a pass and begins a new one on 0x2d0. `beginPassOn` chooses between a
CLEAR pass and a LOAD pass on `t->begunThisFrame`, and picks the framebuffer and
the format-keyed render pass. That is the next thing to instrument: log the
chosen pass, its load op, the framebuffer and the attachment format at every
begin, and compare the two boundaries against a boundary where the value
survives.

### Note (2026-08-06)
## A CONTRADICTION, stated rather than guessed past (2026-08-06)

Two measurements that cannot both describe a simple colour write:

**A. The trace says guest draw 612 changed the pixel.** Pinned to surface 0x2d0
so it samples after every draw (550 samples for 552 issued draws), with the
labelling verified in the code -- `prepared[n-1]`, the draw issued just BEFORE
the sample, with a comment saying naming `prepared[n]` would blame the next one.
The value goes 0 -> (37.09375, 30.984375, 15.90625, 0.125) across that draw.

**B. Rendering ONLY guest draw 612 leaves surface 0x2d0 entirely zero.**
`GEARS_DRAW_ONLY=612` with the surface probe: range 0.0000..0.0000 on every
channel, brightest pixel 0.0000, 0 of 921600 px non-black. So that draw writes
no colour, which is what its RB_COLOR_MASK of 0 says it should do.

So the pixel changes across a draw that writes nothing.

## Everything ruled out so far, so the next session does not redo it

  * the trace mis-attributing across surface switches -- FIXED, rows unchanged
  * the trace's labelling being off by one -- read the code, it is correct
  * R[0x2104] not being RB_COLOR_MASK -- checked against Xenia's register table
  * the `openSurface = 0` sentinel colliding with the real surface at base 0x0
    -- guarded by `!inPass`
  * the pipeline cache key omitting the colour mask -- `OutputMergerState`
    compares `colorMask` first
  * the colour write mask not reaching the pipeline -- `gpu_draw_pipelines.cpp`
    maps the RT0 nibble to `colorWriteMask`, and B above confirms it works
  * the render pass begin -- `GEARS_DRAW_PASS_LOG=1` (new) shows no pass begin
    at that draw at all; the surrounding begins are all LOAD passes on the same
    framebuffer with the same float16 format

## What that leaves

Something OTHER than the draw's colour output changes the surface between two
consecutive samples. The candidates that survive:

  1. The tiling collapse. `CollapseEdramTiling` rewrites the prepared draw list,
     so the Nth issued draw is not the Nth guest draw and a collapsed group may
     execute work the trace attributes to its representative. `GEARS_DRAW_TILED=1`
     restores the faithful path and is the A/B: if the change moves to a
     different draw, the collapse is implicated.
  2. `GEARS_DRAW_ONLY` may not isolate what it appears to. Its own report still
     says "552 of 744 draws issued" while rendering almost nothing, so the
     counter it prints is the PREPARED count, not what was drawn -- B rests on
     the surface probe rather than on that line, but the knob's scope should be
     read before leaning on it again.

### Note (2026-08-06)
## Built a render comparer instead of continuing one hypothesis per iteration

`GEARS_DRAW_TRACE_ALL=<path.tsv>` writes one row per issued draw -- the draw's
identity plus a hash and per-channel statistics of a 32x18 thumbnail of the
surface AFTER it -- and `tools/render_diff.py a.tsv b.tsv` names the first draw
where two runs diverge. `--selftest` covers both classes plus a shifted stream
and a missing file.

### It invalidated the A/B I had planned, on its first run

The plan was collapsed-tiling versus `GEARS_DRAW_TILED=1`. The comparer refuses
to report pixel differences for it:

    THE DRAW STREAMS DIVERGE at row 434: collapsed has guest draw 611,
    tiled has 435. The two runs are not issuing the same draws, so a
    per-row comparison after this point compares different draws.

550 rows against 724. The collapse removes draws, so every row after the first
collapse compares two DIFFERENT draws, and any pixel difference is unattributable.
That would have produced a confident wrong answer had I run it by hand, which is
what the previous iterations were doing.

### An aligned comparison, and what it says

Against `GEARS_DRAW_FORCE_LDR=1` (same draw stream, 550 rows both sides):

    FIRST DIVERGENCE at row 435 -- 435 rows matched before it
    draw 437 (guest 612) surface 0x2d0 ps 272c76c2a6cc
      normal     max 2.9785  3.2402  3.1836
      force_ldr  max 1.0000  1.0000  1.0000

So guest draw 612 is where content first appears on surface 0x2d0, the content
is above 1.0, and it is the first draw at which the widened host format matters
at all. The three preceding rows are 0.0000 on both arms.

### The contradiction stands, and is now sharper

Guest draw 612 is where the surface first holds HDR content, and
`GEARS_DRAW_ONLY=612` renders that draw alone to an entirely empty surface. Both
measurements are now backed by a tool that compares whole surfaces rather than
one texel, and they still disagree.

Note the thumbnail's own limit, which the tool states: 32x18 NEAREST samples 576
of 921,600 pixels, so its 2.98 is not the 37.09 the pixel trace reads at
(368,247) -- it never lands on that texel. The two instruments measure the same
draw, not the same pixel.

### Note (2026-08-06)
## THE CONTRADICTION WAS TWO INSTRUMENT MISUSES, NOT A RENDERER MYSTERY

Several iterations rested on "the pixel changes across a draw that writes
nothing", where the second half came from `GEARS_DRAW_ONLY=612` rendering an
empty surface. Both halves of that were my error.

**`GEARS_DRAW_ONLY` takes an ISSUED index, not the diag table's guest index.**
The same units trap as `GEARS_DRAW_FRAME_STEP_FROM`, in a second knob, found the
same way. It now says so:

    GEARS_DRAW_ONLY=612: matched 0 draw(s) of the 552 this frame ISSUED

Zero draws. The empty surface was a frame with nothing in it, not a draw that
writes nothing.

**And even with the right index the experiment cannot answer the question.**
`DRAW_ONLY` renders its draw OVER THE CLEAR, with nothing before it -- so a draw
that samples a resolve target or a rendered texture has no inputs and produces
black however correct it is. Guest draw 612 is exactly such a draw. Both facts
are in the knob's own output now.

## What the sweep says instead

The interleaved comparer (`GEARS_DRAW_AB`, catalog #82) run over eleven knobs on
`walk_gameplay.gfr`, pinned to surface 0x2d0:

    DRAW_NOTEX            FIRST DIVERGENCE row 435, draw 437 (guest 612)
    DRAW_NORT             FIRST DIVERGENCE row 435, draw 437 (guest 612)
    DRAW_NO_TEX_SIGNS     FIRST DIVERGENCE row 435, draw 437 (guest 612)
    DRAW_FORCE_LDR        FIRST DIVERGENCE row 435, draw 437 (guest 612)
    DRAW_NOBLEND          no divergence
    DRAW_NODEPTH          no divergence
    DRAW_RESOLVE_NOSWAP   no divergence
    DRAW_RESOLVE_BLIT     no divergence
    DRAW_DEPTHONLY_PS     no divergence
    DRAW_NOCULL           no divergence
    DRAW_FIXEDVP          no divergence

Every knob that changes anything changes it at the SAME draw, and all four are
knobs about what a draw SAMPLES or how the surface stores what it writes:
texture content, the resolve-to-texture link, texture sign/gamma decode, and the
widened host format. The seven that touch raster, depth, blend, culling and the
resolve's swap change nothing anywhere in the frame.

So guest draw 612 is the first draw in this frame whose output depends on
textures at all, and the content that appears on surface 0x2d0 at that point is
that draw's shading -- not something that arrived by another route.

## Still open

Its `RB_COLOR_MASK` is 0 and the pipeline maps that to `colorWriteMask` 0
(`gpu_draw_pipelines.cpp`, `attachmentCount = 1`, `pAttachments = &cba`). A draw
that cannot write colour should not be able to put its shading on the surface.
That is now the whole of the question, with every instrument artifact removed
from underneath it.

## The probe does not perturb the render

Checked rather than assumed, since the pinned trace calls `GetSurfaceTarget` and
that CREATES surfaces: the presented frame is byte-identical with and without it
(sha1 `b12c0b413284dce4` both ways, max R68 G84 B76).

### Note (2026-08-06)
## RETRACTION: every "draw 612 / draw 643" note above named the WRONG DRAWS

The pixel trace labelled its rows with `prepared[draws - 1]`, where `draws`
counts DRAWS and `prepared` also contains RESOLVES. The two are different units,
so the label slipped earlier by the number of resolves that had gone past. Every
note in this entry that named guest draw 612 or 643 named a draw that was not
the writer.

I checked that labelling once and passed it: the code carries a comment
explaining why it uses `n - 1` rather than `n`, which addresses an off-by-ONE.
Reading it, I confirmed the off-by-one reasoning and never asked whether `n` and
the prepared index were the same THING. Third units trap of this session, after
`_FRAME_STEP_FROM` and `DRAW_ONLY`.

Fixed by recording the prepared index of the last ISSUED draw at sample time
rather than deriving it from a count.

## The corrected trace, and there is no mystery in it

    after 437 draws  (37.09375, 30.984375, 15.90625, 0.125)  <- draw 615 ps 501ac5d8692bf7b6
    after 468 draws  (1, 1, 1, 0.125)                        <- draw 649 ps c199b399ca818b55
    after 527 draws  (0.29711914, 0.29711914, 0.2685547, 0)  <- draw 716
    after 528 draws  (0.2685547, 0.29711914, 0.29711914, 0)  <- draw 718

    guest 615  surf 0x2d0  fmt 3  mask 15  921600 frags  shaded  ps 501ac5d8692b
    guest 649  surf 0x2d0  fmt 2  mask 15  921600 frags  shaded  ps c199b399ca81

Both are ordinary full-screen draws with a full colour mask. **There was never a
colour-masked draw writing colour.** That whole line of investigation -- six
iterations, a render-pass log, a sentinel hunt, a pipeline-cache-key audit -- was
chasing my own mislabelling. The eliminations it produced are still true; the
thing they were eliminating never existed.

## What the corrected attribution shows, which is a real lead

Draw 649 turns (37.09, 30.98, 15.91) into exactly (1, 1, 1), and its
`color_fmt` is **2 = k_2_10_10_10** -- a FIXED-POINT format. Our
`GuestColorFormatClamp(2)` returns `kRgba`, so the pixel shader's output is
clamped to [0,1] before it is written, which is what the hardware's own
fixed-point target does.

So the clamp itself is faithful. What is NOT faithful is what the surface holds
afterwards. On the console this write stores 10-bit UNORM BITS into EDRAM at
base 0x2d0, and a later draw declaring `k_2_10_10_10_FLOAT` on the same base
REINTERPRETS those bits as 7e3 floats -- a completely different value, not a
clamped one. We keep one widened float16 image per base, so we store the clamped
float 1.0 and any later reinterpretation reads 1.0.

That is exactly the candidate the RE frontier has listed and never tested
(`draw-backend-rt`, gap 1: "all resolve destinations conflated onto ONE host
colour target... needs a per-surface model"), and this frame catches it on a
named draw with a measured before-and-after.

NOT YET ESTABLISHED: that this frame's later draws actually reinterpret base
0x2d0 as a float format after draw 649. The surface's format list for the frame
includes k_2_10_10_10_FLOAT and k_2_10_10_10_FLOAT_AS_16_16_16_16, so the
ingredients are there, but the ORDER has not been checked.
