---
id: I014
kind: instrument
status: trusted
created: 2026-08-06
---

## Instrument

--gears_probe_front_buffer (Xenia fork): reads a guest range out of the GPU's shared-memory buffer at the swap

## Validated by

Run against BOTH classes on 4D5307D5_13457.xtr. Positive: it sees 39,000,806 non-zero bytes across the 512 MiB buffer and names the 16 MiB blocks they are in, so it can report data when data is there. Negative: it reports 0 for the 3.6 MB at the front buffer 1F606000 in the same run and the same moment. Its whole-buffer block map is the built-in control -- it caught two of its own blind spots: probing CPU-side guest memory (which no resolve ever writes) and probing mid-frame (before the command processor's deferred work is submitted, when the whole buffer reads empty). It says 'the probe sees nothing anywhere, so it says nothing' in words rather than printing a bare zero. Cannot distinguish a resolve's output from a texture upload into the same range -- it claims presence only, never provenance.

## Known failure modes

(none recorded yet)
