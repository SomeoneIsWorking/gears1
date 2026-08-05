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

## Caveat, added the same day: rendering is INTERMITTENT

The user pushed back on "it works", correctly: the first launch of the fixed
build sat on a black screen and only a later launch rendered. Counted across
three launches of the SAME binary on the SAME ISO:

    canary5.log     1 swap      in ~120 s   <- black screen, module loaded
    canary6.log  1677 swaps     in   60 s
    canary7.log  4749 swaps     in ~150 s

So the part of this claim that is solid is LAUNCH: all three mounted the disc
and loaded `GAME:\default.xex`. RENDERING stalled in one run of three, and the
stall is not "still loading" -- a single swap in two minutes is a guest that
presented once and stopped.

The cause is NOT known. Do not cite "Xenia renders this title here" as
dependable until it is: quote the 2-of-3 number instead. A run that renders is
not evidence that the next one will, and an oracle that works two times in three
is not yet an oracle.
