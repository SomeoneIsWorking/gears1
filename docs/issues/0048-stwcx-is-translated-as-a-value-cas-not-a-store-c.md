---
id: 48
title: stwcx. is translated as a value CAS, not a store-conditional -- audited, and it cannot affect this title
status: resolved
symptom: cross-thread guest state corrupts; suspected atomic translation divergence
tags: recompiler,atomics,lwarx,stwcx,memory-model,ABA
created: 2026-07-29
updated: 2026-08-22
---

## Root cause

The suspected ring-buffer corruption was not caused by atomic translation: that
ring path uses ordinary guest loads and stores. A whole-image audit found no
reached reservation sequence whose correctness depends on detecting an ABA
change; unpaired conditional stores are reservation-clearing idioms.

The real recompiler defect found during the audit was that reservation loads
were the only guest loads not marked volatile, allowing host optimization to
move or reuse a retry-loop read.

## Resolution

XenonRecomp now emits volatile reservation loads. Regeneration changed exactly
the audited reservation-load sites and no unrelated instruction bodies.

The remaining architectural difference is explicit: a value CAS does not model
reservation loss caused by another thread storing the same value. Supporting
future ABA-sensitive guest code requires a reservation-granule owner on every
guest store; a partial self-invalidation model would not be faithful. Re-run the
atomic audit whenever the title image or recompiler coverage changes.
