---
id: C098
kind: claim
status: holds
created: 2026-08-28
tags: performance,gpu
depends: runtime/gpu_ticket_wait.cpp, runtime/gpu_packet_memory.cpp, runtime/titles/gears1/gpu_ticket_wait_binding.cpp, runtime/titles/gears1/gpu_ticket_wait_state.cpp
---

## Claim

The native Gears 1 operation-kind-3 GPU ticket wait reduces process user CPU

## Evidence

Current Clang headless retained/native controls ran for 35 seconds on the same title path: retained 53.44 s user CPU versus native 32.36 s, while both produced about 30 frames/s; focused wait and state tests passed.

## What would falsify it

A same-binary control with equal produced-frame count and a current profile shows no user-CPU reduction attributable to the retained ticket polling path.
