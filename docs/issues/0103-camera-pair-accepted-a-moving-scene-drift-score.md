---
id: 103
title: camera_pair accepted a moving-scene drift score behind unequal storage UI
status: resolved
symptom: A camera/provenance/input/drift-passing pair still shows NO STORAGE DEVICE on native while the oracle has clean gameplay
tags: oracle,tooling,pairing,ui,diagnostic
created: 2026-08-14
updated: 2026-08-14
---

Root cause: input-fired proves delivery, not title state. START@110,A@260 broke the frame-counter stall but A arrived before native's storage dialog existed, so XamShowDeviceSelectorUI was never called and the title-owned modal remained. pair_score still passed at 2.1 frames because the moving oracle self-curve was permissive. Direct frame inspection falsified C054. Measured draw-stream discriminator: clean scratch/camerapair_ui_repaired_20260814 has 1 draw of VS 5363d0746b3ef666 / PS 501ac5d8692bf7b6; bad scratch/camerapair_character_20260813 has 9, the baseline plus the dialog's eight blended draws. A second clean, turned view has 2 and its corresponding modal class would be 10. tools/ui_state_check.py therefore accepts only the measured clean classes 1/2, explicitly refuses measured modal classes 9/10, and refuses blind 0 or unknown counts; its shipping selftest drives all six outcomes. camera_pair.sh runs it before scoring. The corrected START@110,A@600 route invokes automatic storage selection, captures no modal, matches camera at 0.86 strict thresholds, and scores 1.6 oracle frames of drift.
