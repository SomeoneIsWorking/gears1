---
id: 138
title: Renderer built and emitted detailed per-frame diagnostics on silent replay frames
status: resolved
symptom: A 101-frame headless replay emitted 339050 bytes of renderer logs; untile, resolve-plan, and timing diagnostics were rebuilt for every non-report frame
tags: performance,gpu,draw,diagnostics
created: 2026-08-25
updated: 2026-08-27
---

## Root cause

`FrameDrawInputs::report` already distinguished a requested evidence frame from
ordinary replay frames, but the resolve planner, EDRAM untile pass, and final
timing block ignored it. Every frame therefore allocated and formatted rejection
strings, map entries, group censuses, resolve destinations, and cost breakdowns,
then acquired the logger and wrote them. The untile transformation and its
diagnostic explanation had been coupled even though only the former is shipping
render work.

## What was tried / dead ends

No timing result from the saturated host was accepted. During the confirming
run, unrelated compiler jobs inflated the final reported draw loop to 379 ms,
so that number says nothing about the optimization's frame-time effect.

## Resolution

Normal frames now execute the same resolve planning and untile transformation
without constructing their detailed explanations. A requested report frame
still emits the complete census and timing breakdown. A production-interface
regression runs the same synthetic tiled draw sequence with diagnostics enabled
and disabled and requires identical retained draws, issue count, scissor extent,
and resolve extent.

On the same 101-frame `chapter45_recovered.gfr` replay, the log fell from 339,050
to 83,593 bytes (75.3%). Untile lines fell from 303 to 3 and the combined
untile/resolve/timing pattern from 405 to 5: one complete final report rather
than repeated work on the preceding 100 frames. Evidence:
`scratch/logs/texture_storage_baseline.log` and
`scratch/logs/perf_report_frames_only.log`.

### Reopened (2026-08-27)
A live default-path audit found two remaining per-frame INFO sites: retained-draw accounting and resolve-row alias reuse. A separate checkpoint FString probe also emitted every populated deserialize at INFO.

### Resolution (2026-08-27)
Default-path per-frame draw, resolve-row, and checkpoint-deserialize details now remain available at DEBUG without entering ordinary INFO output. A 30-second Release headless run emits 298 lines / 41,535 bytes and none of those three repeated patterns, versus 3,041 lines / 480,143 bytes before this audit.
