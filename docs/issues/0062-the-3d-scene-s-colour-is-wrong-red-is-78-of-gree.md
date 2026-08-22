---
id: 62
title: The 3D scene colour was wrong and red was suppressed
status: open
symptom: gameplay and menus used the wrong red/blue relationship and residual brightness comparisons were inconsistent
tags: gpu,draw,colour,tonemap,resolve
created: 2026-08-05
updated: 2026-08-12
---

## Fixed root cause

Resolve writes and later texture reads use complementary channel mappings. The
renderer applied the mapping while writing a resolve destination but ignored the
guest texture-fetch mapping when that destination was sampled later. The write
mapping therefore remained visible instead of cancelling at the consumer.

Resolve-target sampled views are now created per guest binding with the requested
component mapping. Identity and non-identity mappings share the same path, and
unsupported depth mappings are refused instead of silently serving an unrelated
view.

## Result

The original red suppression is gone. Menu art uses the intended palette, and
gameplay channel relationships agree with the reference within the established
same-renderer variation band. The fix is per binding; disabling channel exchange
globally would only trade one incorrect consumer for another.

## Why the issue remains open

Several later brightness and midtone conclusions in the forensic record were
withdrawn. They compared different game moments, joined artifacts from different
runs, or relied on extrema that do not describe a frame distribution. The tools
now require shared provenance, a positive same-picture control, and matched input
and UI state before reporting a cross-renderer distribution.

Any remaining colour issue must be reproduced through those gates and localized
to the earliest structurally paired pass. Historical scalar exposure estimates
from unmatched images are not evidence.
