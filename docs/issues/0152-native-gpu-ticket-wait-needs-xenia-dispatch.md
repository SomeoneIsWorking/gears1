---
id: 152
title: Native GPU ticket wait needs Xenia dispatch
status: open
created: 2026-08-28
updated: 2026-09-23
state_items: S005,S006
tags: performance,gpu,synchronization,xenia
---

## Retained contract

Gears 1 operation-kind 3 waits for an exact guest-memory ticket published by GPU
retirement. `runtime/gpu_ticket_wait.*` owns deadline arithmetic, address aliasing,
state decoding, and notified host waiting; `runtime/wait_probe.*` owns watchdog
diagnostics without changing wait semantics.

## Required resolution

Bind the exact guest address through `x360port`, execute the original guest
operation for differential qualification, and prove progress, timeout, and
cancellation. Remove the comparison route after the native operation is accepted.

## Measured cost of the spin (2026-09-23)

The poll step `sub_8222F460` (hint spin, progress check, then `0x827A7B08`) is
the loop behind the ticket, ring-space and progress waits. An experimental
override that slept 20 µs before calling the original body, measured at Act 1's
junction (s280-290 of the gameplay walk), lowered the busiest guest thread from
0.99 to 0.87 cores. Presents per second and process CPU did not change, so the
spin costs host CPU but not frame rate. A notified wait is still the correct
resolution for power use, but it will not help S013's frame rate.
