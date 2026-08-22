---
id: 120
title: Blocking EVENT_WRITE_SHD retirement collapses live gameplay to 5 fps
status: resolved
symptom: menus hold 30 fps but a heavy live scene falls to about 5 fps with zero renderer drops after the GPU retirement fix
tags: performance,gpu,retirement,render-thread,transition
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

The first correctness fix for issue 118 called WaitForRenderIdle inside EVENT_WRITE_SHD. Hardware delays the event memory write until earlier GPU work retires; it does not stop the command processor from consuming later packets. Waiting there serialized the guest command processor behind every 50-200 ms host render. The saved baseline reached only 5.04-5.44 VdSwap frames per second in the heavy scene and reported zero dropped frames because the guest could no longer produce work concurrently.

The asynchronous render thread also dropped every arrival while busy. At a 53 ms render time and a 33 ms producer period, the next arrival always missed the completion boundary and the renderer then sat idle until the following arrival, limiting it to roughly 11 fps despite capacity near 19 fps.

## Resolution

Retirement writes are generation tagged and published only when their accepted render generation completes; the command processor never waits. The render thread keeps one bounded pending frame so it stays saturated, dropping only a second waiter. The large register snapshot was reduced from the command processor storage size of 0x8000 dwords to the authoritative Xenia RegisterFile size of 0x5003. Constant and index scratch storage is reused instead of allocated for every draw.

A 205 second headless menu walk improved the heavy phase from about 5 fps to 17-21 rendered and guest frames per second with generally zero drops and no fault, heap, or GPU-hang markers. Offline warm large-frame draw-loop time fell from about 65 ms to 55-59 ms after allocation reuse. This remains compatibility-path performance; the broader product boundary is the shared GearsUE3 native RHI frontend described in `docs/gearsue3-engine.md`.

## Falsifier

Reopen if an older generation publishes a newer frame completion, if the command processor blocks on live EVENT_WRITE_SHD again, or if the measured heavy path returns to the 5 fps serialized shape.
