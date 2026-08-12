---
id: C035
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

Our shadow-atlas clear rasterises zero fragments, and only on that surface. All twelve clear draws targeting depth base 0x5a0 (vertex shader 760aacf6212e632c, depth func ALWAYS, depth write on) survive clipping and produce no fragment invocations, so the clear writes nothing and the atlas stays at far depth -- which is the whole of issue #97. The same shader produces 58,604 fragments across the other 57 draws of our frame, and the console produces 16,476 across its 59, so neither the shader nor our translation of it is at fault.

## Evidence

scratch/camgate/match/draws.tsv against GEARS_ORACLE_PRIM_STATS=760aacf6212e632c on the fork

## What would falsify it

a capture in which those twelve draws produce fragments, or one in which the console's equivalent draws also produce none
