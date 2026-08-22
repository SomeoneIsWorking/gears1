---
id: 45
title: Checkpoint restore calls through a freed object
status: resolved
symptom: checkpoint restore produced an empty map name and the level loader later called through a destroyed loader object
tags: saves,content,memory,port
created: 2026-07-29
updated: 2026-07-30
---

## Root cause

The host implementation of the asynchronous content-open API read its completion
record from the wrong argument location. That API has more parameters than fit in
the guest argument registers, so the completion record is stack-passed. Reading a
different register made the call appear to complete synchronously.

The title expects the asynchronous-pending result before collecting the final
status. The false synchronous success skipped the checkpoint read while reporting
success, leaving the serialized map name empty. That sent the loader down an
error path where a loader object was destroyed and then used again.

## Fix

Stack-passed guest arguments are now read through one tested ABI helper, and the
content API completes through the correct asynchronous contract. Tests include a
plausible neighbouring value so a future off-by-one argument read cannot pass by
accident.

## Result

The checkpoint is read, the intended campaign map and its streaming packages
load, and the same headless workload continued for several minutes without the
former fault. The save-file name and fallback checkpoint path were not causes.
Issue 44 remained independent and was not fixed by this change.
