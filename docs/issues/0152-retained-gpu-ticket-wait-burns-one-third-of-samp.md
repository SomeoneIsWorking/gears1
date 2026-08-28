---
id: 152
title: Retained GPU ticket wait burns one third of sampled CPU
status: resolved
symptom: The Gears 1 render-ring guest thread spends 33-34% of sampled process CPU in 0x82221A68 -> 0x8222F460 while polling EVENT_WRITE_SHD retirement
state_items: S004,S007
tags: performance,gpu,retirement,native-rhi,synchronization
created: 2026-08-28
updated: 2026-08-28
---

## Evidence

Two independent 30-second release perf captures at commit 29cd2a0 attribute 33.2-34.2% of all process samples to the retained 0x82221A68 ticket wait and its 0x8222F460 adaptive-poll helper, all on guest thread 7. Catalog #58 measured only about 1.3 outer waits per frame, so repeated polling rather than call volume is the cost. The completion producer is the generation-ordered EVENT_WRITE_SHD publication in runtime/gpu_packet_memory.cpp.

The retained contract spins while `(next - ticket) < (next - served)` with unsigned modular arithmetic, where `next = *(device + 10780)`, `servedAddress = *(device + 10768)`, and `served = *servedAddress`. The generic adaptive helper receives operation kind 3 for this path, refreshes its 5,000 ms hang deadline on served-ticket progress, preserves an owning-thread exemption, and calls 0x8222FD78 on a real stall.

## Resolution evidence

The shared packet-memory owner now publishes an address-keyed generation after the authoritative
`EVENT_WRITE_SHD` store. The Gears 1 adapter decodes the exact operation-kind-3 state, blocks on
that generation by default, and invokes the retained helper afterward. The retained helper remains
available through `RECOMP_GPU_TICKET_WAIT=1`, and `GPU_TICKET_WAIT_AB=1` alternates complete wait
episodes between the two paths. Focused tests cover modular deadlines, packet-address aliases,
notification wakeup, state decoding, the owning-thread exemption, and the deadline-refresh path.

On the current Clang build, matched 35-second headless controls measured 53.44 seconds of process
user CPU on the retained arm and 32.36 seconds on the native arm, a 39.5% reduction, while both
produced approximately 30 frames/s. This confirms the expected CPU-side improvement; it does not
claim that the separate title-side 30 Hz limiter is solved.

## Root cause hypothesis

The static recompilation faithfully translates Xenon delay/poll instructions into a host busy loop. The host already knows exactly when EVENT_WRITE_SHD publishes retirement, but the guest waiter has no notified host synchronization seam and repeatedly executes the retained helper until that write arrives.

## Required fix

Add a shared address-keyed GPU packet-memory change signal, notify it only after the authoritative packet-memory store, and bind the exact Gears 1 operation-kind-3 adaptive wait to a blocking native wait. Preserve the retained helper as the super-call and invoke it after each notification or at the exact remaining hang deadline so progress accounting, owning-thread behavior, cleanup timing, and hang escalation stay in the retained authority. Other adaptive-wait kinds continue through retained code. Provide runtime retained/native/A-B controls plus focused notification and deadline tests.

## Falsifier

The fix would be falsified if a current-commit symbol-level profile still attributed the same
polling cost to 0x82221A68/0x8222F460, or if any retained/native semantic run diverged in ticket
completion, hang escalation, or guest progress. The current CPU controls establish the reduction;
the broader native-engine performance target remains tracked by S005/S007.
