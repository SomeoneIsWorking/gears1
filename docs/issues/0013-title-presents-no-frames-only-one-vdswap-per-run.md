---
id: 13
title: Title presents no frames: only one VdSwap per run
status: resolved
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
ROOT CAUSE: KeTimeStampBundle was never updated. The import-variable resolver commits all imported VARIABLES as zeroed 4-byte cells and nothing animates them. KeTimeStampBundle (xboxkrnl.exe:173) is a STRUCT the kernel updates continuously: +0x00 u64 interrupt time (100ns), +0x08 u64 system time, +0x10 u32 tick count ms (the GetTickCount source). The title's ms clock (0x827A9138) is just *(*(0x82000838)+0x10). Bink (0x82AExxxx = RAD library; movie player device at *(0x82BD3754), pump 0x8221B670, BinkWait 0x82AE8C30, timer 0x82AEBB68) paces frames from that clock when no sound system is attached (bink+0x2E8 == 0, measured). Clock frozen at 0 -> elapsed never reaches the 33ms frame period -> movie stuck at frame 1 forever -> its single present was the single VdSwap, and the game thread waits forever on the movie-chain fence (enqueue at 0x8221C1B8, trigger only from 0x8221BAC8 when the chain ends).

EVIDENCE TRAIL: thread backtraces at steady state (render thread 0x82444EF0 idle-spinning on an empty render-command ring at 0x82C0CB24..38 with read==write; game thread parked in event wait from 0x8221C308); movie player state read live (movie index 0 active, bink handle at dev+0xF4, frame counter dev+0x1A0 == 1, frozen across samples); bink struct fields read live; disassembly of the timer chain down to the import variable.

TWO MORE LATENT BUGS found and fixed on the way:
1. GuestHeap had NO lock -- NtAllocate/FreeVirtualMemory race from concurrent guest threads corrupted the std::map (observed 'free of unknown address' storms then SIGSEGV in rb-tree erase). Mutex added.
2. Import-variable name comparisons used bare names but the names carry the '__imp__' prefix -- so the XexExecutableModuleHandle special-case NEVER matched and the module handle was zero. Fixed via prefix strip; that in turn made the title dereference the handle: *(handle+0x58) must point at the XEX header, so InstallExecutableModule() now lays out a module entry + verbatim XEX header copy in guest memory, and RtlImageXexHeaderField is implemented as a real walk of the optional-header table (low-byte 0x00 = value, 0x01 = pointer to inline dword, else offset from header base).

VERIFIED RESULT: movies now play and present at ~30fps -- VdSwap 1 -> 120+ per run (previously always exactly 1). The presentation path works end to end (Bink decode -> draw 0x8221D3A8 -> present 0x824A5170 -> ring kick -> CP -> ISR).

XMSG QUESTION (was the suspected blocker) ANSWERED: the three refused messages are NON-GATING, proven both empirically (movies present with all three still refused) and statically: 0x58004 site 0x828CD530 uses out-value 0 on failure and continues; 0x58020 site 0x828CD8DC frees its buffer and returns soft error 0x65B; 0xB0006 (XUserSetContextEx-shaped, 0x18-byte payload {user, u64, contextId, value}) site 0x82611A98 returns 0x65B on failure. App ids: 0xFA=XMP music player, 0xFB=XGI, 0xFC=XLiveBase. NO handlers were invented; the honest refusal stands.

FURTHER PROGRESS PAST THE MOVIES (each verified by advancing to the next abort): NtCreateMutant/NtReleaseMutant (Bink worker), NtWaitForMultipleObjectsEx (wait-any via try-acquire polling), ExTerminateThread (unwind via exception caught in GuestThreadMain), InterlockedPopEntrySList/InterlockedFlushSList (real 8-byte CAS on the console SLIST layout -- pushes are inlined guest code, so a host lock would not exclude them), XMACreateContext/XMAReleaseContext (real zeroed 0x40-byte context in physical memory; REFUSING creation was measured intolerable -- the audio device inits 'successfully' anyway and crashes in its Update dispatching through null members; the missing decoder is the audio frontier and is logged loudly), and recompiler function-table miss at 0x825B4EB8 (vtable target the boundary analysis missed; registered in config/gears.toml functions list, size 0x12AC to the shared restore tail at 0x825B6160; found via host jump-to-0 with ctr=0x825B4EB8).

CURRENT FRONTIER (open): with all the above, the title finishes the movies and begins REAL SCENE RENDERING on the render thread, then SIGSEGVs in sub_82544148 (chain 0x825550A8 -> 0x82553258 -> 0x82554B68 -> 0x82544148) reading *(r24+368) with r24 = 0x3D8D7C97 -- a float bit-pattern where an object pointer belongs. Something upstream in the render-command stream carries garbage; suspects include D3D resource creation paths against the null GPU (readbacks/queries returning zeros) or another boundary/translation defect. Needs its own focused investigation.
