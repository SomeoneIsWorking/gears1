---
id: C022
kind: claim
status: holds
created: 2026-08-07
tags: determinism,harness,oracle,clock
depends: runtime/vd_null_gpu.cpp#__imp__VdSwap
---

## Claim

A deterministic guest clock requires deterministic SCHEDULING of the guest's threads; no choice of clock anchor fixes it while the title's threads are host threads the OS preempts.

## Evidence

GEARS_WORK_TRACE logs the guest's kernel-call count at every present. Two runs, same binary, same input script, fixed-step clock: the count matches at 1 of 22,209 common presents (the first), differs by 7.91% by present 3, 23.29% by present 5, and stays ~12% apart to present 20,000. The count is summed across host-scheduled guest threads, so any clock anchored to observed guest activity inherits the nondeterminism -- true of retired blocks or instructions equally. Four anchors tried and each failed separately: present (boot deadlock), vblank-freerun (picture freezes), vblank-paced (starves after 3 presents), guest work (this).

## What would falsify it

if the divergence is dominated by ONE thread whose work is host-paced (the audio pump submits at a fixed 187.5 Hz against real time) rather than by scheduling in general -- exclude that thread's calls from the count and re-measure before accepting the structural conclusion
