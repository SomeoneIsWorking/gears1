---
id: 44
title: Intermittent guest progress stall near the campaign transition
status: open
symptom: the audio pump stops advancing and presentation plateaus while the process stays alive; other runs pass the same point
tags: hang,nondeterministic,guest,audio,blocker
created: 2026-07-28
updated: 2026-08-28
---

## Current status

The original frequency estimate is withdrawn. Early runs conflated several
different failures, the reproduction harness treated a timed-out live process as
clean, and a duplicated input script sometimes stopped in a menu instead of
reaching the intended workload. The classifier and input source were corrected.
Later headless runs, including a long menu-to-gameplay run, did not reproduce the
stall.

## What is established

The title's game and render threads can both enter its render-command producer.
The corrected detector distinguishes operating-system thread identities and
shows that both entrants are guest execution, not an accidental host producer.
In the long run that exercised both entrants, they alternated but never overlapped
inside the critical region, and the run did not stall. Therefore the former
lost-update explanation is unsupported and serializing the ring would be a
speculative workaround.

A pure-virtual dispatch and a separate stale-object fault were also observed in
historical failing runs. They are different mechanisms. Instrumentation can now
identify the abstract object family if the pure-virtual fault recurs, but no such
fault occurred in the confirming long run.

## Next evidence required

Reopen the root-cause investigation only on a new run classified as stalled by
the corrected harness. Preserve the first failing object-lifetime report and the
per-subsystem progress counters from that run. Do not infer a ring race merely
from the existence of two producers; an observed overlap or corrupted commit is
required.

### Note (2026-08-28)
Fixed the distinct-thread detector's reporting state: it retained only the immediately previous identity, so ordinary alternation emitted an error on every producer entry and generated multi-megabyte logs. It now remembers the set of observed OS thread IDs and reports each newly discovered identity once. This does not change the established zero-overlap conclusion.
