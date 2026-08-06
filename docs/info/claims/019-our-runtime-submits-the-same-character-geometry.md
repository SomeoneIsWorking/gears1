---
id: C019
kind: claim
status: holds
created: 2026-08-06
tags: render,catalog-77,oracle
depends: runtime/vd_null_gpu.cpp
---

## Claim

Our runtime submits the same character geometry the reference does: for both skinned vertex shaders compared, our per-frame bind counts land on Xenia's modal values. The black character is therefore NOT a missing-draw or CPU-side submission problem

## Evidence

Per-frame bind counter added to the Xenia fork (VulkanCommandProcessor::UpdateBindings, keyed on a reproduction of our runtime's FNV-1a 64 over the big-endian ucode bytes), logged at IssueSwap, wall-clock Act 1 walk. vs 15cbc482459fe5b7: oracle 849 of 879 frames at 2 draws; ours bright/black/character_auto all 2. vs 8354e5cc00c0a98c: oracle modal 4 (x475) and 5 (x404) of 1337 frames; ours 4, 4, 5. Counts vary with characters on screen (oracle spread 1..9), so equality of modes is strong evidence rather than proof, and two of the frame's four skinned shaders remain unmeasured on the oracle side.

## What would falsify it

a matched-moment comparison showing our bind count below the oracle's at the SAME guest frame would falsify it; so would either of the two unmeasured skinned shaders (0xf3e9368c1bb68ecc, 0x57997d3a9dbfd37e) differing
