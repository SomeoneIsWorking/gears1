---
id: C021
kind: claim
status: holds
created: 2026-08-06
tags: determinism,harness,oracle,clock
depends: runtime/guest_clock.cpp#GuestClockNanoseconds
---

## Claim

A guest clock that advances only on presented frames DEADLOCKS this title at boot: the first frame cannot be reached because the title spins in guest code waiting for time before it ever presents.

## Evidence

GEARS_GUEST_CLOCK_STEP_NS=16666667: 0 frames written in 100 s against 9 for the real-clock control on the identical input script. Guest alive throughout (17,000 audio frames, 7.8M kernel calls/s). Stall reporter: guest-7 and gpu-isr 'running guest code, not in any kernel call' -- a spin, so not a timed wait anything can intercept. Logs scratch/oracle/clock/{realclock,fixedclock}.log. This CONTRADICTS catalog #84's proposed fix, which prescribes exactly this design.

## What would falsify it

if the deadlock is actually caused by a clock reader still on the real host clock (so the guest sees time go BACKWARDS when it engages) rather than by the spin -- check by logging mftb monotonicity across the mode switch before concluding the design is at fault
