---
id: 6
title: Registers read at a watchpoint stop are wrong
status: resolved
symptom: register values captured when a gdb watchpoint fires disagree between identical runs
tags: debugging,method
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-07-22)
A hardware watchpoint reports AFTER the storing instruction retires, by which point the recompiled code has moved on and reused registers. Two runs of one script gave r9=0xA06F032C,r10=1 and r9=0x1,r10=4. Use a conditional BREAKPOINT on the store (stops before it, registers valid), or log only $pc and the watched memory. This produced one retracted conclusion in this project.
