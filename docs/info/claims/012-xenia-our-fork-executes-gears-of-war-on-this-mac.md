---
id: C012
kind: claim
status: holds
created: 2026-08-05
tags: oracle,harness
depends: extern/xenia/src/xenia/ui/imgui_drawer.cc#InitializeFonts, extern/xenia/src/xenia/ui/vulkan/vulkan_immediate_drawer.cc#CreateTexture
---

## Claim

Xenia (our fork) executes Gears of War on this machine, windowed: disc mounts, default.xex loads, ~28 fps of real swaps

## Evidence

scratch/oracle/run/canary6.log: 1677 'IssueSwap: front buffer' lines in a 60 s window, loading this title's own surfaces (864x864 k_24_8 depth, 322x182 and 128x128 k_16_16_16_16_FLOAT); canary5.log: 'Loading module GAME:\default.xex', content resolved under \WarGame, 14 guest threads. User confirmed correct graphics on screen. Required fork commit 9946714 (font-atlas crash killed the UI thread).

## What would falsify it

a run of extern/xenia's xenia_canary on this ISO that reaches no 'Loading module GAME:' line, or produces zero IssueSwap lines in 60 s -- and note this says nothing about whether Xenia's OUTPUT is correct, only that it runs
