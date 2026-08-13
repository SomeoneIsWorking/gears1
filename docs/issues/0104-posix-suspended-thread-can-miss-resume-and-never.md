---
id: 104
title: POSIX suspended thread can miss Resume and never execute
status: resolved
symptom: The oracle creates Main XThread but intermittently never logs XThread::Execute; GPU drains 123 startup swaps and the title never polls input or presents again
tags: oracle,xenia,threading,race,launch
created: 2026-08-14
updated: 2026-08-14
---

Root cause in extern/xenia threading_posix.cc ThreadStartRoutine: it published State::kSuspended and unlocked, then in a second critical section set suspend_count_=1 and waited. Emulator::LaunchPath could call XThread::Resume in the gap; Resume saw state suspended but count 0, returned false, then the child set count 1 and waited forever. Fix publishes state and suspend_count atomically under state_mutex and waits in that same critical section. Negative evidence: multiple camera_pair boots stopped at guest frame 123 with Main XThread created but no XThread::Execute. Positive discriminator after rebuild: three consecutive fresh oracle launches each logged Main XThread Execute and captured guest frame 200. The aggregate Xenia build linked xenia_oracle successfully; an unrelated xenia-ui-window-vulkan-demo target later failed its pre-existing HID link.
