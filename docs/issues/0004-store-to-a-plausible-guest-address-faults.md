---
id: 4
title: Store to a plausible guest address faults
status: resolved
symptom: SIGSEGV writing to a low guest address like 0x0032003C that nothing allocated
tags: memory,aliasing
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-07-22)
Guest code converts virtual to physical by masking the top 3 bits (clrlwi rD,rS,3), so a pointer handed out as 0xA0320000 comes back as 0x0032003C and must be the same bytes. Physical RAM is now one memfd mapped MAP_SHARED at 0x0/0xA0000000/0xC0000000/0xE0000000.
