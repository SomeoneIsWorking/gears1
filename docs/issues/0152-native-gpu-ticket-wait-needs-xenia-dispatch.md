---
id: 152
title: Native GPU ticket wait needs Xenia dispatch
status: open
created: 2026-08-28
updated: 2026-09-04
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
