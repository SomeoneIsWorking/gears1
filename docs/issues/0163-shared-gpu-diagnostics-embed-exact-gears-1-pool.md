---
id: 163
title: Shared GPU diagnostics embed exact Gears 1 pool and representative-draw policy
status: resolved
symptom: runtime/vd_null_gpu.cpp contains the exact 0x82000868 worker-pool global, per-CPU event layout, representative vertex shader hash, fetch index 95, and stride 12 used by diagnostics
state_items: S003
tags: architecture,title-boundary,gpu,diagnostics,shader,gearsue3
created: 2026-08-30
updated: 2026-08-30
---

## Root cause

The command processor acquired diagnostic policy while Gears 1 was the only linked title. Generic
interrupt dispatch and PM4 capture therefore became the accidental owner of the title's D3D worker
global/layout and the exact shader/fetch/stride tuple chosen as its representative draw. A second
title would inherit those bindings merely by composing the shared command processor.

## Required resolution

Keep packet decoding, register capture, and interrupt dispatch in the shared GPU owner. Move the
exact worker-pool/event layout and representative-draw selection into a required linked-title
profile with shared validation. One executable must link exactly one strong profile definition;
malformed profiles must refuse, and the Gears 1 product must preserve both normal execution and the
enabled constant/draw-capture behavior.

## Resolution

### Resolution (2026-08-30)
Root cause fixed: runtime/vd_null_gpu.cpp embedded Gears 1 worker-pool/event layout and representative-draw shader/fetch/stride policy. A title-neutral validated GPU diagnostics profile now owns resolution, while runtime/titles/gears1/gpu_diagnostics_profile.cpp is the sole exact binding. Evidence: focused profile tests; all 94 non-quality CTests; 131/131 Clang quality units; enabled diagnostic run reached frame 1080 and selected shader 0x5363d0746b3ef666/fetch 95; normal diagnostics-disabled headless run reached frame 960; no invalid-profile diagnostic; exact values absent from shared runtime.
