---
id: 54
title: The scripted menu walk stops on the difficulty screen and the capture looks successful anyway
status: resolved
symptom: tools/capture_gameplay_frame.sh finishes and its frames carry ~170 draws instead of 800+, i.e. the capture is of a menu, not gameplay
tags: harness,input,capture,instrument
created: 2026-08-04
updated: 2026-08-04
---

## Symptom

The scripted walk into Act 1 ran to completion, rendered thousands of frames and
left screenshots -- of the SELECT DIFFICULTY menu. Frames carried 165-175 draws
throughout; an Act 1 gameplay frame carries 800+. Nothing in the run reported a
problem: the input script fired all twelve of its steps on schedule.

## Cause

The walk's six presses assume every press lands on a screen that is ready for
it. A press delivered during a menu transition is swallowed by the title, and
the walk has no way to notice -- it is a timed sequence, not a state machine.
Adding four spare A presses after the last one (75/90/105/120 s) reached
gameplay: 844-draw frames.

The spare presses are harmless where they land: on the level, after it has
begun.

## Why it matters more than it looks

This is an instrument that fails silently in the direction of looking like it
worked. Two sessions' worth of "gameplay" screenshots from this script would be
menu screenshots, and the number that distinguishes them -- the draw count -- is
in the log but nobody has to read it to believe the capture.

Watch the draw count, not the exit status.
