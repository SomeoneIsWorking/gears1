---
id: 9
title: Ghidra decompiles Xbox 360 functions to a single call
status: investigating
symptom: decompiled body is just 'WARNING: Subroutine does not return' plus one call to a 0x828dxxxx address, no real code
tags: ghidra,re,method
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
Cause: Xbox 360 functions call __savegprlr_NN / __restgprlr_NN helpers in prologue and epilogue. Those helpers restore LR and effectively return to the caller's caller, which the decompiler cannot follow, so it treats them as non-returning and discards the whole body. Confirmed here: FUN_82761ca8 and FUN_82766f68 both decompile to a single call to FUN_828d2818 / FUN_828d280c.

### Note (2026-07-22)
NOT a noreturn flag -- checked directly, hasNoReturn() is already False on 0x828d2818, 0x828d280c, 0x828d27e0 and 0x828d2830, so clearing it changes nothing. The inference is inside the decompiler, not the function DB. Next approaches: patch each helper's bytes to a bare blr (0x4E800020) in the Ghidra program only, so it becomes a trivially-returning stub; or define them as inline/thunk; or exclude the prologue call from the function body.

### Note (2026-07-22)
Working infrastructure, ready to reuse: image imports and fully analyses as PowerPC:BE:64:A2ALT-32addr at base 0x82000000 (~8 min), project at build/ghidra/gears, scripts in tools/ghidra_scripts (DecompDump.py, FixNoReturnAt.py). Helper addresses come from config/gears.toml.

### Note (2026-07-22)
PARTIAL PROGRESS. Stubbing the helper ranges removed the 'Subroutine does not return' warning -- FUN_82761ca8 now decompiles to 'FUN_828d2818(); return;' instead of a noreturn call, so the diagnosis was right. But the write lands as 00000000 rather than blr (0x4E800020): a Jython str literal does not convert to a Java byte[] and silently writes zeros, and switching to jarray.array with signed values did not fix it either -- a peek still reads 00000000 after the run reports success. Zeros disassemble to an invalid instruction, so flow analysis stops and recreating the function yields a body of only 8 bytes.

### Note (2026-07-22)
So two things are now known: the approach is correct (the noreturn inference is what was discarding bodies, and stubbing addresses it), and the remaining obstacle is purely mechanical -- getting four real bytes written into the Ghidra program and persisted. Worth checking whether headless actually saves memory edits made by a -postScript, since the byte value never changes across runs while other edits appear to stick. Next attempt should verify the write inside the same script by reading the bytes straight back before exiting.
