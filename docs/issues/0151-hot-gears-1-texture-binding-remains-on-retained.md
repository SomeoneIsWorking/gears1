---
id: 151
title: Hot Gears 1 texture binding remains on retained PPC path
status: dead-end
symptom: SetTexture 0x82220858 executes tens of thousands of times during gameplay and still performs descriptor merge, dirty-state update, and retirement handling through generated PPC
state_items: S004
tags: performance,native-rhi,texture,binding,override,gears1
created: 2026-08-28
updated: 2026-08-28
---

## Root cause

The apparent hotspot was call volume, not execution cost. `SetTexture` is frequent because the
title rebinds state across its draw stream, but the retained descriptor merge and dirty-state work
does not consume a meaningful share of process CPU on the measured gameplay workload.

## What was tried / dead ends

An existing release profile was checked against the call census: 233,377 calls over 2,940 frames
(about 79.4 calls/frame) accounted for only about 0.01% self CPU. The profile instead concentrated
33–34% of process samples in the retained GPU ticket wait, so moving this setter first would not
address the measured performance cause.

## Resolution

### Dead end (2026-08-28)
Profile evidence rejects SetTexture as the next performance target: it runs about 79.4 times/frame (233,377 calls over 2,940 frames) but accounted for only about 0.01% self CPU in an existing release capture. The retained GPU ticket-wait chain 0x82221A68 -> 0x8222F460 instead accounted for roughly one third of sampled CPU. Native SetTexture remains valid S004 migration work, but doing it before the wait would optimize call volume rather than measured cost.

### Note (2026-08-28)
Profile evidence established this is a valid native-engine migration seam but not the next performance target: about 79.4 calls/frame yet roughly 0.01% sampled self CPU, versus 33-34% in the GPU ticket wait. Preserve this finding to prevent optimizing call volume in place of measured cost.
