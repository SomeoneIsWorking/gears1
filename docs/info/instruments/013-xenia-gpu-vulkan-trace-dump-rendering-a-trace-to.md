---
id: I013
kind: instrument
status: DISTRUSTED
created: 2026-08-05
distrusted_on: 2026-08-05
---

## Instrument

xenia-gpu-vulkan-trace-dump (rendering a trace to PNG)

## Validated by

FAILED its validation on 2026-08-05: fed a trace XENIA ITSELF captured, of a frame the live headless harness rendered correctly (22k colours, 76% non-black), the dump writes a UNIFORMLY BLACK PNG and exits 0. Verified the draws execute (0 'Failed in backend'), all 18 resolves run and write 3.7 MB, the swap finds its 1280x720 texture, and the guest-output image reports a SUCCESSFUL refresh -- and it is still black. A 2 s wait after playback does not change it, so it is not a simple race.

## Known failure modes

(none recorded yet)

## DISTRUSTED 2026-08-05

Renders a KNOWN-GOOD trace as uniform black. This is the control arm: Xenia's own capture of a frame its own live emulator drew correctly comes back 0.000 mean, 1 distinct colour, exit 0. So every 'our trace renders black' result measured THIS TOOL, not our trace -- gfr_to_xtr is exonerated of the blackness (its index-count bug was real and separately fixed). Do not use it as an oracle until it can render a Xenia-produced trace; use tools/xenia_oracle (claim C013) instead.

> Every result this instrument produced is suspect until it is re-validated.

## Narrowed 2026-08-06 (still DISTRUSTED)

Still not usable as an oracle, but the blackness is no longer unexplained, and
three candidates are eliminated by measurement rather than argument (`catalog.py
show 79`): the gamma ramp is present and sensible (255 of 256 entries non-zero),
the presenter's capture path is the same one `tools/xenia_oracle` uses
successfully, and the guest-output refresh runs and submits its own work.

What is measured instead: at the swap, the shared-memory buffer holds 39 MB of
non-zero data and **zero** in the front buffer's 3.6 MB. The trace's own
`MemoryRead` snapshot of that page, decoded out of the `.xtr`, is 1.22%
non-zero -- so the trace never carried the picture. `readback_resolve` defaults
to `none`, so the CPU-side page a capture snapshots is never written by the GPU
resolve that produces the front buffer.

Re-validate by recapturing with `--readback_resolve=full` and dumping THAT. It
needs a live Xenia run against the disc image.
