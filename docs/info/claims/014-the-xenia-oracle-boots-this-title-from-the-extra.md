---
id: C014
kind: claim
status: holds
created: 2026-08-06
tags: 
---

## Claim

The Xenia oracle boots this title from the EXTRACTED TREE (scratch/game/default.xex); the disc image was never required, and oracle_compare.sh's GEARS_ISO refusal is what gated every 'needs the disc / needs a person' note about the oracle

## Evidence

Ran it: xenia_oracle --store_shaders=false --target=scratch/game/default.xex --oracle_out=scratch/oq5 --oracle_seconds=110 --oracle_interval=10 --oracle_input=START@25+8 produced 11 consecutive 1280x720 PNGs through the boot logos, the red loading screen and the main menu, with no window and GEARS_ISO unset. A second 60s run reached the same screens. tools/oracle_compare.sh now falls back to the XEX and prints which arm it booted from.

## What would falsify it

The oracle stops booting the extracted tree -- e.g. the title starts demanding disc-only content, or scratch/game is incomplete. Falsified the moment a --target=<xex> run produces no frames.
