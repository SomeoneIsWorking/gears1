---
id: 11
title: SIGSEGV loading the UE3 package Core.xxx
status: investigating
symptom: fault at ppc_recomp.86.cpp:14620 in sub_8255D3D8, on 'lwzx r0,r12,r0' -- the jump-table load feeding a bctr, with switch index r9 out of range
tags: recompiler,switch,packages
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
Call path: sub_8243A718 (worker) -> sub_82588320 -> sub_823335A8 -> sub_8255D3D8. Reached after the title plays its intro movies and opens CookedXenon/Core.xxx, so this is inside package loading.

The faulting instruction is the original table load that fed the bctr. The recompiler ALSO emitted a 'switch (ctx.r9.u64)' immediately after it with resolved 'goto loc_...' cases, so the load itself is dead in the recompiled output -- but it still executes and faults because r12 + r9*4 lands outside mapped memory.

Do NOT 'fix' this by eliding the dead load. On hardware the same load would fault identically with an out-of-range index, so the index is genuinely wrong and something upstream produced it. Eliding the load would convert a visible fault into silent wrong-branch behaviour, which is the exact failure mode the switch-table work earlier in this session was undoing.

Registers at fault: r11=0x440c0001 r31=0x4A9A1B69 r30=0x4A9A0000 r3=0. Next step is to find who computes r9 in sub_8255D3D8 and why it exceeds the table bounds.
