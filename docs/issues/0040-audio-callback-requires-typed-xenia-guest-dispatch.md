---
id: 40
title: Audio callback requires typed Xenia guest dispatch
status: open
created: 2026-07-28
updated: 2026-09-04
state_items: S006,S009
tags: audio,callback,xenia
---

## Root cause

The host audio pump owns cadence and callback registration, but the retired call
mechanism was deleted with the old CPU product. Invoking the title callback now
requires an authenticated `x360port` execution context and typed guest call API.

## Retained behavior facts

The callback cadence is 187.5 Hz (`48000/256`). It receives its registration
context in guest `r3`, signals the title audio worker, and waits on two dispatcher
objects. Correct processor-number propagation is required because the worker uses
per-CPU state. These contracts remain in the native audio/kernel sources and tests.

## Required resolution

Route the callback through Xenia with explicit context lifetime, bounded exit,
dispatcher wake semantics, and reason-labelled dynarec/fallback counters. A
fallback execution must not be treated as gameplay or performance evidence.
