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
