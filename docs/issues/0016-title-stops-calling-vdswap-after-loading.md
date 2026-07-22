---
id: 16
title: title stops calling VdSwap after loading
status: investigating
symptom: post-load: guest VdSwap calls freeze (572, delta 0 over 40s) while the command processor keeps executing swap packets at ~12/s; guest threads still executing engine code, not blocked
tags: gpu,presentation
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
MEASURED, and note the earlier reading of this state as 'continuous submission that looks like asset loading' was too generous.

Two DISTINCT log lines exist and must not be conflated -- I conflated them once and drew a wrong conclusion:
  '[gpu] VdSwap: swap packet at ...'  = the GUEST calling VdSwap
  '[gpu] swap packet: front buffer ...' = the COMMAND PROCESSOR executing one
Over a 40 s window in the post-load state: guest 572 -> 572 (frozen), CP 8234 -> 8727 (+493, ~12/s).

HYPOTHESIS TESTED AND REJECTED: that the ring consumer overshoots the write pointer and laps the ring, re-executing stale packets. A guard was added that reports and resynchronises when a packet claims more dwords than have been written -- it fired ZERO times. The ring is not lapping. The guard is kept because a ring consumer must never read past the write pointer, but it is honestly a no-op here and fixed nothing observable.

REAL DEFECT FOUND in our own VdSwap implementation: the swap packet is written into a 64-dword reservation INSIDE the guest's frame indirect buffer, and that guest memory persists. A later submission that reuses the same buffer without calling VdSwap still carries the old packet, so the CP executes it again. That is why CP swap packets vastly outnumber guest VdSwap calls. The frame-boundary signal is therefore unreliable and needs a validity marker the guest cannot leave stale -- for example stamping a sequence number the CP checks against the submission it is executing, so a stale copy is recognisable.

STILL UNEXPLAINED, and this is the actual question: why the guest stops calling VdSwap at all. Thread backtraces in this state show guest threads ACTIVELY EXECUTING engine code (sub_825550A8 -> sub_82553258 -> sub_82553EA0 -> sub_82492610 -> sub_825A4300 -> sub_822306A0), not blocked on any wait. So it is running and choosing not to present, rather than being stuck. Next: determine what that call chain is doing and what condition gates the render/present path.
