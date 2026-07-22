---
id: 3
title: Guest segfaults on a load relative to r13
status: resolved
symptom: SIGSEGV at PPC_LOAD_U32(ctx.r13.u32 + 256) very early in boot
tags: threads,boot
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Resolution (2026-07-22)
r13 holds the current thread's KPCR on Xbox 360 and offset 0x100 is current_thread. The console kernel sets it up before a title runs; we started the guest with r13=0. Fixed by allocating a KPCR/thread/TLS/stack block per guest thread.
