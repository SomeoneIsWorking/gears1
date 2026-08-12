---
id: C032
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

A hand evaluation of shader 0xf3e9368c1bb68ecc in Python reproduces the GPU's clip verdict: draw 293's vertices land inside the frustum (|x|,|y|<=w, 0<=z<=w) and draw 294's land far outside (x/w=9.6, z/w<0). Our translated vertex shader is therefore not miscomputing these positions.

## Evidence

docs/issues/0091; inputs read from the held firing frame in scratch/clipmath/run.log; model follows the disassembly instruction by instruction

## What would falsify it

a GPU-side readback of oPos (transform feedback) disagreeing with the CPU model for either draw
