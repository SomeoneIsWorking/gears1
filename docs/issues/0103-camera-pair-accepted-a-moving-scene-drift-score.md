---
id: 103
title: camera_pair accepted a moving-scene drift score behind unequal storage UI
status: resolved
symptom: A camera/provenance/input/drift-passing pair still shows NO STORAGE DEVICE on native while the oracle has clean gameplay
tags: oracle,tooling,pairing,ui,diagnostic
created: 2026-08-14
updated: 2026-08-14
---

Root cause: input-fired proves delivery, not title state. START@110,A@260 broke the frame-counter stall but A arrived before native's storage dialog existed, so XamShowDeviceSelectorUI was never called and the title-owned modal remained. pair_score still passed at 2.1 frames because the moving oracle self-curve was permissive. Direct frame inspection falsified C054. Measured draw-stream discriminator: clean scratch/camerapair_ui_repaired_20260814 has one isolated draw of VS 5363d0746b3ef666 / PS 501ac5d8692bf7b6; bad scratch/camerapair_character_20260813 has nine split into one baseline draw plus the dialog's eight consecutive blended draws. A second clean, turned view has two isolated draws, not one run of two. tools/ui_state_check.py therefore classifies the consecutive-run shape: it accepts runs no longer than one (including zero occurrences in a materially different scene), refuses the measured eight-draw suffix, and refuses unknown runs of two through seven. Its shipping selftest drives clean one, clean one-plus-one, measured one-plus-eight/eight, zero, unknown three, and nine-isolated cases. The real modal and clean corpora are also driven. camera_pair.sh runs it before scoring. The corrected START@110,A@600 route invokes automatic storage selection, captures no modal, matches camera at 0.86 strict thresholds, and scores 1.6 oracle frames of drift.
