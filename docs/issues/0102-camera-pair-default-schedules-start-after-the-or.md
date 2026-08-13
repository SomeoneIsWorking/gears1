---
id: 102
title: camera_pair default schedules START after the oracle stops presenting
status: resolved
symptom: A default camera_pair run waits its full timeout with 0/N oracle frames and no camera constants, while the oracle remains alive at guest frame 123
tags: oracle,tooling,pairing,input,deadlock
created: 2026-08-13
updated: 2026-08-14
---

Root cause: frame-driven input uses guest_swap_count, but this boot reaches a static title frame and stops presenting at frame 123 until START. The old first event was START@450, so the counter could never reach the event that would make it advance. Negative: the shipping default route ran 360 s, stayed at frame 123 with one draw per frame, dumped 0/8 frames and correctly refused. Positive discriminator: oracle-only START@110,A@260 reached guest frame 900, captured a 1156-draw gameplay frame, and fired both presses.

The first correction, `110:START 260:A`, fixed reachability but was not an equal-state pair: A arrived before native's storage dialog, leaving NO STORAGE DEVICE over native gameplay (issue #103). A second launch defect was independent: the POSIX oracle could lose `Resume()` before its suspended count was published (issue #104). Finally, an eight-frame START pulse could fall wholly between the title's input polls even after the thread race was fixed. The shipping default therefore holds START continuously from f90 through f210 (`90:START~120`) and presses A at f600. `menu_walk.sh` generates overlapping oracle pulses for the hold while native gets one press/release interval; its last-frame calculation includes the release. The held-input oracle discriminator logged Main XThread execution, START at f90 and a capture at f250. C055 retains the clean f110/f600 paired evidence; C056 records the clean held-route turned pair.
