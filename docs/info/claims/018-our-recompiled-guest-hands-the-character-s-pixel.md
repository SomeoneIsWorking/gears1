---
id: C018
kind: claim
status: holds
created: 2026-08-06
tags: render,catalog-77,oracle
depends: runtime/gpu_draw_uniforms.cpp
---

## Claim

Our recompiled guest hands the character's pixel shader (f662d670789bfac0) the SAME float constants Xenia's emulation does: seven of ten byte-identical, and the three that differ (c3/c4/c5) are the camera basis, both sets orthonormal to six decimals

## Evidence

Direct comparison 2026-08-06. Oracle side: a one-shot dump added to VulkanCommandProcessor::UpdateBindings writing scratch/oracle/ps_consts.txt, keyed on a reproduction of our runtime's FNV-1a 64 over the big-endian ucode bytes (Xenia's own ucode_data_hash is a different function and matched nothing). Identification verified by a census mode that lists every distinct shader the oracle binds -- it contains f662d670789bfac0 at 90 dwords and 10 consts, matching our runtime exactly. Our side: GEARS_DRAW_PS_CONSTS=f662d670789bfac0 on bright.gfr. Identical: c0,c1,c2,c6,c7,c8,c9 -- including the normal-map decode (2,-1,0,0), the luminance weights (0.11,0.3,0.59) and the output gate (0.7,1,0.8,8).

## What would falsify it

this covers ONE pixel shader at ONE moment; a view-independent constant found to differ on another shader or another frame would falsify the generalisation, and the c3/c4/c5 agreement is untested because the two runs face different directions
