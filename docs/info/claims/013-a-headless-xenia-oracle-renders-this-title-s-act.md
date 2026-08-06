---
id: C013
kind: claim
status: holds
created: 2026-08-05
tags: oracle,harness
depends: tools/xenia_oracle/main.cpp, tools/xenia_oracle/scripted_input.cc
reconfirmed: 2026-08-06
verified_at: 2026-08-06
---

## Claim

A headless Xenia oracle renders this title's Act 1 gameplay with no window anywhere in the path

## Evidence

tools/xenia_oracle + scratch/oracle/run/oracle11.log: 6254 swaps, 0 crashes, 115 scripted presses, 7 of 7 captures; frames from 120s on measure 22k-36k distinct colours and ~76% of pixels above 8/255, and show Act 1. Needs SDL audio (SDL_AUDIODRIVER=dummy) -- the nop APU makes the guest crash in its own render-driver callback at PC 0x825F39F4.

## What would falsify it

a run of tools/xenia_oracle on this ISO whose captured frames are uniform, or which logs a CRASH DUMP -- and note this says nothing about whether Xenia's pixels are CORRECT, only that they exist

## Re-confirmed 2026-08-06

Re-verified 2026-08-06 after the trace-wait and shm-unlink changes to tools/xenia_oracle/main.cpp. Two headless runs off the disc image with no window and no desktop dependency: scratch/oracle/live (7 frames at 30s intervals) and scratch/oracle/live2 (4 frames at 60s intervals), all 1280x720 PNGs. The gameplay ones are a lit scene, not black -- 120s mean R23.0 G23.5 B17.0 at 100% non-black, 180s mean R25.2 G25.5 B18.6 at 99.8%. The renderer inside it reported 1280-2125 draws recorded per frame with 0 dropped. Still true after the changes, and the changes are the reason it was re-checked.
