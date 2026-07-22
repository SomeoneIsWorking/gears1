---
id: 5
title: A watchpoint on a guest address never fires though memory changes
status: resolved
symptom: gdb hardware watchpoint set on a guest address reports no writes, yet the value is clearly modified
tags: debugging,method
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-07-22)
Two causes, both giving the same symptom. (1) Physical RAM is aliased at four virtual windows; a hardware watchpoint watches a VIRTUAL address, so writes through another alias never fire it -- watch all four. (2) Changing the allocator moves every address, silently invalidating watch addresses derived earlier -- re-derive after any allocator change.
