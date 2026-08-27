---
id: 147
title: Color-write gamma setter is hot but native replacement is not faster
status: resolved
symptom: Render-target descriptor changes from 0x302D0 to 0xC02D0 after binding and the suspected state setter rate/owner is wrong
tags: performance,rhi,native-override,render-target,gears1
created: 2026-08-28
updated: 2026-08-28
---

## Root cause

The earlier seam census mislabeled `0x82229B28` as one of nine low-rate sampler
setters. The unexplained descriptor change actually comes from this function:
it is a color-write gamma/sRGB state setter called about 167 times per active
scene frame. It maps target format pairs 2↔10 and 3↔12, updates both the bound
object and device descriptor, and marks dirty bit 37.

## What was tried / dead ends

A native semantic implementation passed a 256-call transactional audit,
including two real format transitions, but did not improve call cost. A clean
same-run alternating measurement reported 40 ns native versus 30 ns retained
median over 5,000/5,000 calls. Default native activation was rejected.

The first strong-wrapper build also inlined the full diagnostic executor and
reserved 2.9 KiB of stack on every call, even with diagnostics disabled. The
shipping wrapper initializes Lucent configuration once on its first call, then
reads one atomic configuration byte and tail-jumps directly to the retained
body; the large executor is non-inlined and reachable only under explicit
native/audit/A/B/timing flags. A rejected static-initialization attempt kept the
wrapper small but ran before Lucent configuration was usable, so every runtime
flag silently read false; the final audit arm falsified and removed it.

## Resolution

`color_write_gamma_state.h` is the independently testable semantic owner and
`color_write_gamma_override.cpp` retains runtime native, recomp, audit, A/B,
and timing arms. The retained body remains the default because it won the
measurement. The binder-paused write watch remains available to attribute
future descriptor writers without counting legitimate rebinds.

### Resolution (2026-08-28)
Grounded 0x82229B28 as the hot color-write gamma state owner; retained its audited native A/B seam but kept recomp as default because 40 ns native lost to 30 ns retained, and reduced the initialized default wrapper to one atomic configuration-byte branch plus a tail jump.
