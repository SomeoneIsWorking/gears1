---
id: 14
title: Render thread SIGSEGV in sub_82544148 during first real scene render (r24 holds float garbage)
status: open
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
After the startup movies finish (see #13), the render thread begins executing real scene-render commands and crashes: sub_82544148 (called via 0x825550A8 -> 0x82553258 -> 0x82554B68) executes 'lwz r3,368(r24)' at guest 0x82544870-ish with r24 = 0x3D8D7C97, a float bit-pattern where an object pointer belongs, i.e. the render command or the object graph it references carries garbage.

Context that is KNOWN GOOD at this point: command processor + interrupt + fence protocol (entry #12), movie presentation at 30fps (#13), XMA contexts exist but never decode, one recompiler-missed function 0x825B4EB8 registered and regenerated (its absence produced an earlier jump-to-0 crash in the same chain -- if more jump-to-0 crashes appear here, check for more missed vtable targets FIRST, the diagnostic signature is host frame #0 == 0x0 and ctx.ctr holding a valid-looking guest code address).

Not yet investigated: whether r24's garbage comes from a D3D resource-creation path returning zeros against the null GPU (readbacks, GPU-written queries), from uninitialised guest data the console's kernel would have filled, or from a translation defect. Start by identifying what object *(r24+368) belongs to in sub_82544148 (the fields at +152/+296/+368 are passed to sub_8254EE20) and walking back where r24 was loaded.
