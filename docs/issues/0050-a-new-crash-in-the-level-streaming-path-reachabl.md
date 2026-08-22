---
id: 50
title: A new crash in the level-streaming path, reachable only now the campaign loads
status: open
symptom: some campaign runs make an invalid virtual call while gathering streamed level components
tags: crash,streaming,level-load,nondeterministic,post-45
created: 2026-07-30
updated: 2026-07-30
---

## Root cause boundary

The immediate crash mechanism is established. The streaming gather maps a byte
from one array into an index for a separate component table. Healthy runs produce
the expected small index; failing runs produced an out-of-range value. The title
does not validate that mapped index before using the selected table entry as a
polymorphic object, so unrelated transform data is interpreted as a virtual-call
target.

What makes the two arrays diverge is not established. Adding a clamp would hide
the violated ownership invariant and is not an acceptable fix.

## Reproduction status

The crash was intermittent: three observed failures across dozens of bounded
runs, followed by a long clean streak. Checked-call fault context distinguishes
this issue from the resolved checkpoint bug and preserves the bad mapped index
when it recurs.

## Next evidence required

Trace the producer and lifetime of both index arrays through a failing level
transition, then repair the state transition that lets them become inconsistent.
Do not special-case the observed bad index or skip the affected component.
