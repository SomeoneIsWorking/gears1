---
id: 12
title: title reports the GPU is hung
status: open
symptom: guest DbgPrint: 'Breaking into the debugger. The GPU is hung and can't be recovered without doing a cold boot' -- reached after startup completes, with exactly one VdSwap
tags: gpu,graphics
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
Reached only after async Xam completion was fixed; before that the title stalled earlier and never got here. The diagnosis is the title's own and it is CORRECT -- the runtime's GPU is an explicit null.

What exists: VdInitializeRingBuffer records the ring, RetireRingBuffer reports the whole ring as consumed by writing the write pointer to the read-pointer write-back address, and a host thread drives the graphics interrupt callback at 60 Hz (source 0 = vblank).

Concrete gap found: VdSetSystemCommandBufferGpuIdentifierAddress stores the address in g_systemCommandBufferGpuIdentifier and NOTHING EVER WRITES TO IT. On hardware this is a fence the GPU updates so the title can see which command buffer has been consumed. A title polling it sees no progress, which is consistent with declaring the GPU hung.

DELIBERATELY NOT GUESSED: what value belongs there. The identifier echoes back a submission id the title supplies, and inventing a monotonically increasing number would be a value chosen to make the symptom disappear rather than one derived from the protocol -- the exact shape of bandaid this project bans. Determine what the title writes into the system command buffer first, then echo that.

Note also that the interrupt callback is only ever raised with source 0 (vblank), never a command-completion source. Whether the title needs a completion source is unverified and should be checked rather than assumed.

The real fix is a command processor that parses the ring buffer, which is a substantial subsystem and not a stub.
