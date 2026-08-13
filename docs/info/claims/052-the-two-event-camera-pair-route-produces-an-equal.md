---
id: C052
kind: claim
status: holds
created: 2026-08-13
tags: oracle,comparison,pairing,input,ui
depends: runtime/xam_notify.cpp, runtime/xam_user.cpp, tools/camera_pair.sh, tools/pair_score.py
---

## Claim

The two-event camera-pair route produces an equal gameplay/UI state on native and oracle, and the resulting pair passes the measured temporal-drift gate.

## Evidence

scratch/camerapair_short_ui_20260813: one camera_pair.sh invocation with f450 START/f600 A. Provenance MATCH and frozen camera digest 5836bee14961d066; both input arms fired; oracle camera f901; native matched at 0.61 camera thresholds. Native against oracle f901 scores 0.7496, between the console's +1-frame 0.7715 and +2-frame 0.6035 points, or 1.1 frames under the 3.0-frame gate. Directly decoded buffers show the same door and no overlay. A pre-fix native frame carries NO STORAGE DEVICE; a post-fix capture after XN_SYS_UI close shows it absent and CHECKPOINT present. tests/test_xam_notify.cpp drives the queue's positive and wrong-area negative classes.

## What would falsify it

A repeat shipping camera_pair.sh run with the default route in which either side retains an overlay, an input arm does not fire, provenance/camera validation fails, or the cross score cannot be priced within three frames on that run's measured console self-curve.
