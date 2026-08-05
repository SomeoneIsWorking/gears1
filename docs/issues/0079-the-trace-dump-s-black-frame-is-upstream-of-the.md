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
