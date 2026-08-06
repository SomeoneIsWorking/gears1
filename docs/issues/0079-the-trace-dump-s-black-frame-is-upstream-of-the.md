---
id: 79
title: The trace dump's black frame is upstream of the presenter: the front buffer is empty in shared memory
status: investigating
symptom: xenia-gpu-vulkan-trace-dump writes a uniformly black PNG (exit 0) for a trace of a frame that rendered correctly
tags: oracle,trace,xenia,instrument,present,black
created: 2026-08-06
updated: 2026-08-06
---

Continues I013's distrust record. The tool renders a trace XENIA ITSELF captured
as uniform black; this narrows where the picture is lost, with a new instrument
that reads the GPU's shared-memory buffer back rather than reasoning about it.

## Ruled OUT, measured, on `scratch/oracle/xenia_traces/4D5307D5_13457.xtr`

  * THE GAMMA RAMP IS NOT EMPTY. The swap shader puts every channel through the
    guest's LUT, and an all-zero table would turn any frame black with opaque
    alpha -- which is exactly the observed output. Measured at the swap: 255 of
    256 entries non-zero, first `00000000 00100401 00300C03`, last `3FFFFFFF`.
    A sensible ramp, so this is not it.
  * THE PRESENTER'S READBACK IS NOT BROKEN. `CaptureGuestOutput` is the same
    call `tools/xenia_oracle` uses to write the frames that DO come out correct
    (claim C013), so the path that turns the guest output image into a PNG is
    exercised by a working instrument.
  * THE SWAP FINDS ITS TEXTURE and the guest-output refresh runs and submits
    (`EndSubmission(true)` is inside the refresher, so it is not unsubmitted
    work waiting for a frame that never comes).

## What IS measured

At the swap, after every submission has completed, the shared-memory buffer --
the buffer the swap texture is LOADED from -- holds 39,000,806 non-zero bytes
across 512 MiB, and **zero** in the 3.6 MB at the front buffer `1F606000`.

So the picture is not in the place the swap reads it from. Everything downstream
of that is behaving.

## The trace's own front-buffer snapshot is stale, and that is a capture defect

Decoded straight out of the `.xtr` (snappy, in `MemoryRead` for `1F606000`,
3,768,320 bytes): **45,912 non-zero bytes, 1.22%, mean 1.10**. The trace does
not carry the picture either.

The mechanism: `readback_resolve` defaults to `none`, so a render-to-texture
resolve writes the GPU's shared-memory buffer and never the CPU-side guest page.
Xenia's trace capture snapshots the CPU-side page, so for a front buffer that
only the GPU ever writes, what it records is whatever stale bytes were there.

NOT YET ESTABLISHED, and it is the next thing to settle: whether recapturing
with `--readback_resolve=full` makes the snapshot real and the dump correct.
That needs a live Xenia run, i.e. the disc image mounted, which this session did
not have.

## The other resolve destinations, same frame, same moment

    137A0000+5242880:53841   13ED8000+2621440:1962412  13CA0000+2293760:0
    14158000+1146880:796909  12D97000+3768320:0        134C7000+1548288:1119208
    1312F000+3768320:0       1F557000+540672:10487     1F606000+3768320:0

Some destinations hold data and some hold none, which is the open question. A
range holding data does not prove a resolve put it there -- a texture upload
from guest memory lands in the same buffer.

## Our own synthesised trace is a different failure

`scratch/oracle/traces/swap_frame.xtr` now reaches a swap (so the missing swap
event is fixed), but its whole shared-memory buffer holds 213,279 non-zero bytes
in two blocks, and all ELEVEN of its resolve destinations read zero. A frame
whose geometry and textures are absent resolves empty targets, which is
consistent with what comes out. So `gfr_to_xtr`'s memory emission is the gap on
our side, and it is separate from the capture defect above.

## The instrument, and the two blind spots it caught in itself

`--gears_probe_front_buffer` (Xenia fork, `VulkanCommandProcessor::
ProbeSharedMemoryRange`) reads a guest range out of the shared-memory buffer at
the swap and reports it, along with every resolve destination of the frame and a
whole-buffer block map. The block map is the control: a range reading zero and a
probe that cannot see anything are otherwise identical, and it says so in words
when it finds nothing anywhere.

It needed that control twice:

  * The FIRST version probed CPU-side guest memory and reported 1.2% non-zero
    for a frame that had definitely rendered. Resolves do not write there at
    all, so it was measuring the wrong memory and reading exactly like a real
    negative. (Its number later turned out to match the trace's stale snapshot
    exactly, which is how the capture defect was found -- but that was luck,
    not the measurement it claimed to be.)
  * The SECOND version also probed at each resolve. Every such probe reported
    the ENTIRE 512 MiB empty, because a probe that submits its own command
    buffer mid-frame runs before the command processor's deferred work is
    submitted. The whole-buffer control is what exposed that; the range count
    alone would have read as "the resolve wrote nothing".

### Note (2026-08-06)
## OUR trace: the draws run, and the COLOUR targets are black (2026-08-06)

The earlier reading of our own trace was wrong in a way worth recording: it was
built by an OLD `gfr_to_xtr`. Regenerated with the current one, from
`scratch/frames/courtyard.gfr`:

  * **The index-count defect is gone.** 0 "index buffer only containing 0"
    warnings, against 665 before. Every draw fetches its indices.
  * **726 of 744 draws are RECORDED as real `vkCmdDrawIndexed` calls.** The 18
    missing are exactly this frame's 18 resolves, which go to `IssueCopy`. So
    the accounting is complete and no draw is lost.
  * `IssueDraw` has TWO paths that drop a draw and return SUCCESS ("no effect",
    "nothing to draw"); both are now counted and both are ZERO here. That
    matters because a frame taking either path renders nothing and logs nothing.
  * Every resolve finds a render target to dump (0 "NO render target owns") and
    every copy has a shader. Both of those were silent early-outs that write
    ZEROS over the destination rather than skipping, so they looked identical to
    a resolve of a black target from the destination memory.

And yet, measured at the swap on the same run:

    DEPTH  destinations: 0BA50000+2621440:1960979   0BCD0000+1146880:796626
    COLOUR destinations: 0BDF0000, 0C2F0000, 0CB91000, 0C520000, 0C7F9000,
                         006E4000, 00311000  -- every one of them ZERO

**The same colour/depth split is in Xenia's OWN trace** (`4D5307D5_13457.xtr`):
its two depth destinations carry 1,962,412 and 796,909 bytes, and its front
buffer and several colour destinations carry nothing.

So the resolve machinery is working -- it resolves depth correctly through the
same code -- and the colour render targets are BLACK at resolve time. The draws
execute and write no colour.

That is the one remaining question, it is the same question for both traces, and
it is no longer about the presenter, the gamma ramp, the swap, the index buffer,
the resolve, or the dump: it is about what the colour attachment holds when the
draws have run.

### Note (2026-08-06)
## The live positive control, and the readback hypothesis REFUTED (2026-08-06)

With the disc image available, the arm that was missing all along.

### Live, headless, `--readback_resolve=full`, the probe sampling every 200 swaps

    front buffer 1F606000: 2,729,246 of 3,686,400 bytes non-zero (74.0%), mean 25.2
    resolve destinations: EVERY ONE non-zero --
      137A0000:3,746,188  13ED8000:1,963,277  13CA0000:1,526,200  14158000:796,776
      1312F000:2,764,592  134C7000:1,119,237  1F557000:55,187     1F606000:2,759,966
    whole buffer: 91.6 MB non-zero
    draws: 1280 recorded, 0 dropped

So the instrument reports a full front buffer and full colour destinations when
they are full. Steady across four samples 200 swaps apart.

### The same title, one of ITS OWN traces, played back

Captured at 150 s in that run, 42 MB, complete:

    front buffer 1F606000: 747 of 3,686,400 bytes non-zero (0.0%), mean 0.03
    DEPTH   13ED8000:1,962,383  14158000:797,064  134C7000:1,119,197 -- correct,
            and within 0.1% of the LIVE numbers for the same destinations
    COLOUR  137A0000:774  13CA0000:570  12D97000:0  1312F000:0  1F606000:747
    whole buffer: 39.3 MB non-zero
    draws: 1286 recorded, 0 dropped by either silent success path

Playback reproduces the frame's GEOMETRY to within a rounding error -- the depth
destinations match the live run's byte counts -- and produces no colour at all.

### REFUTED: `readback_resolve=full` at capture time does not fix it

That was this entry's stated next step, and it is wrong. The trace above was
captured under `--readback_resolve=full` and dumps exactly as black as one
captured without it. So the stale CPU-side front-buffer snapshot is REAL (it is
still 1.22% non-zero in the older trace) but it is NOT the cause of the black
dump -- the colour never gets into the shared-memory buffer in the first place,
which is upstream of anything a snapshot could fix.

### Ruled out: the captured frame being a black one

Checked rather than assumed, because it would make all of the above vacuous. The
live run's own frames either side of the capture are a lit scene: 120 s mean
R23.0 G23.5 B17.0 at 100% non-black, 180 s mean R25.2 G25.5 B18.6 at 99.8%.

### Where that leaves it

The question is now sharp and has both arms measured: **why do Xenia's colour
resolves write nothing during trace playback when the same code writes megabytes
live, on the same title, from a trace of a real lit frame whose geometry replays
correctly?** Depth through the same resolve path is right; the draws are all
recorded; the dumps all find their render targets; the copies all have shaders.

The trace carries 68.1 MiB of MemoryRead, spread across every 16 MiB block, so
it is not obviously missing guest memory wholesale.

### Note (2026-08-06)
## `readback_resolve` is NOT the cause -- the entry's leading hypothesis is dead

This entry's "NOT YET ESTABLISHED, and it is the next thing to settle" was
whether `--readback_resolve=full` makes the dump correct. It has now been run,
on a trace of OUR OWN capture (`tools/gfr_to_xtr.py scratch/frames/bright.gfr
scratch/traces/bright.xtr --present frame`, 844 draws, 2878 guest pages,
179.9 MiB emitted):

    readback_resolve=full   100.0% pure black
    readback_resolve=fast   100.0% pure black
    readback_resolve=none   100.0% pure black

All three identical. **The cvar is not the mechanism.**

### And our converter's memory emission is NOT the gap either

The entry records "`gfr_to_xtr`'s memory emission is the gap on our side",
measured on an old synthesised trace whose whole shared-memory buffer held
213,279 non-zero bytes in two blocks. That is no longer true. The probe at the
swap reports the buffer holding **32,769,057 non-zero bytes spread across nine
16 MiB blocks**, and playback records 826 draws with 0 dropped. The trace
carries the frame.

### Where the picture is actually lost, measured

`--gears_probe_front_buffer=1` at the swap, on that trace:

    shared-memory probe [swap swap #0]: 00311000 (3686400 bytes, tiled):
        0 non-zero (0.0%), mean 0.00

immediately after the fork's own instrumentation reports

    IssueCopy: entry, dest 00311000
    DumpRenderTargets: EDRAM base 720 -> 1 rectangle(s)
    IssueCopy: resolved 3768320 bytes at 00311000

So a render target WAS found to own the EDRAM range (the dump did not take its
"copy zeros" path), the copy reports 3,768,320 bytes written, and the
destination in shared memory is entirely zero. The resolve's write is not
reaching the buffer the swap reads.

Of the frame's eighteen resolve destinations the probe lists, three are
non-zero -- 0BA50000, 0BCD0000, 0C520000 -- and those are addresses our own
`kMemoryRead` blocks loaded at trace start. **Not one destination is non-zero
because a resolve put it there.** So this is not specific to the front buffer:
no resolve in trace playback lands in shared memory.

### What that makes the next step

Not a cvar and not our converter: it is inside the fork's Vulkan resolve path
under trace playback, between `IssueCopy` reporting a written length and the
shared-memory buffer holding the bytes. That is emulator work in
`extern/xenia`, and it is the whole of what stands between this project and a
per-draw ground truth for catalog #77 -- which, after this session's
eliminations, is the only measurement #77 has left.

Recorded with both hypotheses killed so the next session starts here rather
than re-running the cvar.
