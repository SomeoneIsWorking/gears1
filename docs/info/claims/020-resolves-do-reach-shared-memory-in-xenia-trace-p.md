---
id: C020
kind: claim
status: holds
created: 2026-08-06
tags: oracle,trace,resolve
depends: extern/xenia/src/xenia/gpu/vulkan/vulkan_command_processor.cc#IssueCopy
---

## Claim

Resolves DO reach shared memory in Xenia trace playback; the early ones land and the LATE ones write zeros over them, so a probe taken only at the swap reads zero and reports a false negative.

## Evidence

GEARS_PROBE_AFTER_RESOLVE=1 on scratch/traces/bright_delta.xtr: 6 of 18 destinations 58-84% non-zero measured immediately after each resolve's own submission (0BA50000 74.8%, 0C7F9000 84.3% at #7, emptied to 0.0% by #9). Xenia's OWN capture 4D5307D5_13457.xtr shows the identical pattern (early 69-98%, late 0.0%, front buffer 0.0%), so this is Xenia trace playback, not gfr_to_xtr. Logs in scratch/oracle/deltatest/.

## What would falsify it

if the after-resolve probe is reading a buffer the resolve does not write, or if EndSubmission(true) is not actually flushing the copy before the readback -- both would make the POSITIVE readings spurious rather than the negative ones
