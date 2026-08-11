---
id: 100
title: Editing a shell script while an instance of it is running corrupts that run
status: resolved
symptom: layer_capture.sh printed its OLD retry message and then 'line 225: he: command not found'
tags: tooling,workflow,capture
created: 2026-08-11
updated: 2026-08-11
---

## What happened

`tools/layer_capture.sh` was edited while a 20-minute paired capture was still
executing it. The running shell had already read past the edited region, so it
printed the message the OLD text produced -- and then, when it read the next
block, it resumed at a BYTE OFFSET that now landed in the middle of a rewritten
line:

    the oracle dumped nothing and did NOT stall at frame 1, so this is not
    the flaky boot. Not retrying -- read theirs.log.
    tools/layer_capture.sh: line 225: he: command not found

`he` is the tail of a word the edit moved. The script ON DISK was fine
(`bash -n` clean); what was corrupted was the running process's view of it.

## Why it matters here

`sh` reads a script incrementally by file offset rather than loading it whole,
so an edit under a running instance is not "the next run picks up the change" --
it is arbitrary code substitution in the current one. The failure mode is
silent-looking: the run keeps going and does something that is neither the old
script nor the new one.

## The rule

DO NOT EDIT A SHELL SCRIPT WHILE AN INSTANCE OF IT IS RUNNING. Wait for it, or
copy it aside and edit the copy. This session's captures run for 12-20 minutes,
which is exactly long enough to forget one is in flight.

Nothing was lost this time: the run had already failed (the oracle wedged before
gameplay) and its result was going to be discarded. That is luck, not a
mitigation.
