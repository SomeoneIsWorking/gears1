---
id: C013
kind: claim
status: holds
created: 2026-08-05
tags: oracle,harness
depends: tools/xenia_oracle/main.cpp, tools/xenia_oracle/scripted_input.cc
---

## Claim

A headless Xenia oracle renders this title's Act 1 gameplay with no window anywhere in the path

## Evidence

tools/xenia_oracle + scratch/oracle/run/oracle11.log: 6254 swaps, 0 crashes, 115 scripted presses, 7 of 7 captures; frames from 120s on measure 22k-36k distinct colours and ~76% of pixels above 8/255, and show Act 1. Needs SDL audio (SDL_AUDIODRIVER=dummy) -- the nop APU makes the guest crash in its own render-driver callback at PC 0x825F39F4.

## What would falsify it

a run of tools/xenia_oracle on this ISO whose captured frames are uniform, or which logs a CRASH DUMP -- and note this says nothing about whether Xenia's pixels are CORRECT, only that they exist
