---
id: I062
kind: instrument
status: trusted
created: 2026-08-28
---

## Instrument

GEARS_NATIVE_RHI_OBSERVE resolve seam

## Validated by

Focused tests require exact resolve decoding, reject a draw packet outside the
owned command span, and cover a retained command-buffer allocation transition;
real headless runs reported 540/540 resolve matches through frame 540 and no
resolve refusal through frame 2280 after the transition fix.

## Known failure modes

The retained body can replace the command buffer behind its device write pointer
when the per-buffer limit is crossed. The observer must keep the search bounded
and only inspect the new allocation after a lower post-call pointer proves the
transition.
