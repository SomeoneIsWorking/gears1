---
id: 14
title: Render thread SIGSEGV in sub_82544148 during first real scene render (r24 holds float garbage)
status: resolved
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

### Note (2026-07-22)
BOTH SCENE-PHASE FAILURES RESOLVED. They were TWO INDEPENDENT problems; causal order established by log-line ordering and by fixing them separately: the transient 'GPU is hung' episodes came first in every run and the SIGSEGV later, but the crash was NOT caused by the hang -- after the hang fix (below) the crash still reproduced alone, and after the switch-table fix both are gone.

PROBLEM 1 -- scene-phase 'GPU is hung' (returned symptom, same escalation path as entry #12; NOT a regression, the movie path stayed correct). Root cause: VdSwap. D3D's Present reserves 64 dwords in the command buffer and passes their address as r3 (verified live: r3 = CB pointers, r5 = 0xA030A008 sys-buffer id). On hardware the KERNEL fills that block with the swap PM4; our VdSwap wrote nothing, so whatever stale CB bytes sat there were parsed as packets. Measured in a captured IB: at the reserved block the stream carried 0x...C2000000, which parses as a 513-word TYPE3 and blows the walk past the frame's two fence packets (both healthy and broken IBs have their second fence header at word 0xA65; broken ones have the 64-dword unfilled block at 0xA1F..0xA5E). Skipped fences -> served ticket stalls ~5s until a later frame's fence rescued it -> escalation prints. Diagnosed from ring/IB dumps at the hang plus fence-provenance logging added to the CP (EVENT_WRITE_SHD now logs which buffer+offset it executed from). FIX: __imp__VdSwap writes a runtime-private TYPE3 packet (opcode 0x7F, count spanning the full 64-dword reservation, data[0] = front buffer address read via r8) and the CP handles it as the frame boundary. Same contract Xenia uses (its XE_SWAP packet). NOT invented values: the reservation size is measured from the streams, and the packet encoding is kernel-GPU private by construction.

PROBLEM 2 -- SIGSEGV in sub_82544148, r24 = 0x3D8D7C97 (same float bit pattern every run). Root cause: A MIS-RECOMPILATION I INTRODUCED in the previous session. The 'missed function' 0x825B4EB8 is NOT a function: it is case 3 of an UNRECOGNISED JUMP TABLE -- 0x825B48B8 dispatches via lwzx r0,r12,r0 / mtctr / bctr at 0x825B48F4 over a 16-entry table at 0x825B48F8 (cases 0x825B4938..0x825B6020). Case blocks share the parent's frame and end in the shared restore tail; emitting one as a standalone function makes it run the epilogue against a frame it never built, restoring the caller's r14-r31 from garbage -- r24 came back as a float and sub_82544148's 'lwz r3,368(r24)' crashed. (r24's real value is the global 0x82C0DE04; the original jump-to-0 symptom that motivated the wrong fix was the same table with NO function entry at the case address.) FIX: reverted the functions entry in config/gears.toml (with a warning comment: never register case labels as functions); added the switch to scratch/config/gears_switch_tables.toml (base = the bctr 0x825B48F4, r = 8 -- the index register; r0 only holds the scaled offset). Regenerated + full rebuild.

VERIFICATION: 340+ s run: 0 'GPU is hung' (was 10-16 per run), no SIGSEGV (was 100% reproducible within ~150-250 s), 540+ VdSwaps, title keeps running -- after the movies it settles into continuous submission (~90 interrupt handshakes/s, all retired) without presenting, threads active across engine/render code: looks like map/asset loading against the null GPU. Whether it eventually needs real rendered output to reach the menu is the next question.

OPEN ITEMS / WORKFLOW: (1) scratch/config/gears_switch_tables.toml is NOT git-tracked (scratch/ is ignored) but now contains a manual, load-bearing entry -- it needs a committed home or the fix is machine-local. (2) Recompiler regen printed pre-existing 'switch case jumping outside function' errors for tables at 0x8251E794 and 0x825E7648 -- unrelated to this change but worth auditing. (3) The post-load no-present steady state is uncharacterised.

### Resolution (2026-07-22)
Two independent defects: VdSwap leaving the title's 64-dword reserved swap block unwritten (desynced the CP, skipped frame fences, transient GPU-hung escalations) and a jump table at 0x825B48F4 unknown to the recompiler (case block previously mis-registered as a function corrupted nonvolatile registers). Both fixed and verified: 0 hangs, no crash, title runs 340+ s.
