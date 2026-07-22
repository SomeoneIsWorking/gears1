---
id: 16
title: title stops calling VdSwap after loading
status: open
symptom: post-load: guest VdSwap calls freeze (572, delta 0 over 40s) while the command processor keeps executing swap packets at ~12/s; guest threads still executing engine code, not blocked
tags: gpu,presentation
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
MEASURED, and note the earlier reading of this state as 'continuous submission that looks like asset loading' was too generous.

Two DISTINCT log lines exist and must not be conflated -- I conflated them once and drew a wrong conclusion:
  '[gpu] VdSwap: swap packet at ...'  = the GUEST calling VdSwap
  '[gpu] swap packet: front buffer ...' = the COMMAND PROCESSOR executing one
Over a 40 s window in the post-load state: guest 572 -> 572 (frozen), CP 8234 -> 8727 (+493, ~12/s).

HYPOTHESIS TESTED AND REJECTED: that the ring consumer overshoots the write pointer and laps the ring, re-executing stale packets. A guard was added that reports and resynchronises when a packet claims more dwords than have been written -- it fired ZERO times. The ring is not lapping. The guard is kept because a ring consumer must never read past the write pointer, but it is honestly a no-op here and fixed nothing observable.

REAL DEFECT FOUND in our own VdSwap implementation: the swap packet is written into a 64-dword reservation INSIDE the guest's frame indirect buffer, and that guest memory persists. A later submission that reuses the same buffer without calling VdSwap still carries the old packet, so the CP executes it again. That is why CP swap packets vastly outnumber guest VdSwap calls. The frame-boundary signal is therefore unreliable and needs a validity marker the guest cannot leave stale -- for example stamping a sequence number the CP checks against the submission it is executing, so a stale copy is recognisable.

STILL UNEXPLAINED, and this is the actual question: why the guest stops calling VdSwap at all. Thread backtraces in this state show guest threads ACTIVELY EXECUTING engine code (sub_825550A8 -> sub_82553258 -> sub_82553EA0 -> sub_82492610 -> sub_825A4300 -> sub_822306A0), not blocked on any wait. So it is running and choosing not to present, rather than being stuck. Next: determine what that call chain is doing and what condition gates the render/present path.

### Note (2026-07-22)
DECISIVE: the title is NOT loading in this state. Counting from the last guest VdSwap call (log line 5901 of 177190), there are ZERO [fs] lines afterwards. So roughly 97% of the run is post-presentation execution with no file access and no frames.

This refutes the earlier characterisation of the post-load steady state as 'asset/map loading against the null GPU'. There is no loading. The title is executing engine code in a loop, doing no I/O and presenting nothing.

Combined with the thread backtraces (guest threads ACTIVELY RUNNING sub_825550A8 -> sub_82553258 -> sub_82553EA0 -> sub_82492610 -> sub_825A4300 -> sub_822306A0, not blocked on any kernel wait), the shape is a SPIN on a condition that never becomes true -- not a deadlock and not progress.

Next step is to identify that condition: sample the guest thread PCs repeatedly to confirm the loop is tight and locate its head, then decompile it (verifying against raw disassembly, since the decompiler misleads on any function whose body Ghidra failed to rebuild). The likely shape, given everything else in this port, is a wait for GPU-produced state that the null GPU never writes -- but that is a hypothesis and has not been tested.

### Note (2026-07-22)
ROOT CAUSE FOUND AND FIXED. The spin is D3D occlusion-query GetData: sub_822306A0 (r3=query object, r4=out; query+4=type, 9=D3DQUERYTYPE_OCCLUSION; +20=ticket; +24=encoded physical block list; +144=count). Its loop: (1) inline ticket check against pool+0x2A10/0x2A1C -- the query's submission HAD retired (measured ticket 0x481 vs served 0x483), so not the blocker; (2) decode each entry to a physical report block and treat it as pending while its first four words hold the sentinel 0xFFFFFEED. Measured live: the polled block at physical 0x0D6C9000 held four 0xFFFFFEED words, zeros elsewhere -- never written.

On hardware the writer is TYPE3 EVENT_WRITE_ZPD (opcode 0x5B): writes a 0x20-byte xe_gpu_depth_sample_counts record (Total/ZFail/ZPass/StencilFail A+B pairs, LITTLE-endian -- the guest reads them with lwbrx and computes ZPass(END at slot+0) minus ZPass(BEGIN at slot+0x20)) at the record selected by register RB_SAMPLE_COUNT_ADDR (0x2325). Contract confirmed against extern/xenia (xenos_zpd_report.h: same sentinel constant, record/slot layout, GetRecordBase = addr & ~0x1F). Our CP silently skipped opcode 0x5B, so the sentinel never cleared.

FIX (runtime/vd_null_gpu.cpp): CP handles EVENT_WRITE_ZPD by zero-filling the 0x20-byte record at (reg 0x2325 & ~0x1F) -- a GPU that rasterises nothing passes zero samples, and the write clears the sentinel; and EVENT_WRITE_EXT (0x5A, used by the type-10 screen-extent query issued by the adjacent code at 0x82230634 as 0xC0015A00 + initiator 26 + addr|1) writing the six 8-in-16-swapped extent halfwords with the conservative full-surface extent, per Xenia's handler. Also fixed the stale-swap-packet defect from this entry: VdSwap now stamps a sequence number (data[1] = frame count) into the swap packet and the CP ignores packets whose sequence is not newer than the last executed (measured 10836 stale skips in one run).

VERIFIED: guest VdSwap advanced past the old freeze point (572 frozen -> 630 and climbing), 165k ZPD + 87k EXT events served, zero 'GPU is hung', no crash over a ~15-minute run; movie phase unaffected.

NEW FRONTIER, measured but NOT fixed -- frame rate is ~0.2 fps: each presented frame performs ~784 interrupt handshakes (submission -> INTERRUPT -> ISR queues work at pool+0x2A94 -> D3D worker thread must process before the stream's WAIT on 0x30B004 releases), and the worker wakes mostly by its own 30 ms KeWait timeout (avg latency ~7 ms/handshake => ~5.5 s/frame; present sits in the ticket lock just under the 5 s escalation, so no hang prints). The fast wake the hardware had is UNESTABLISHED: the ISR provably discards the inner callback's returned per-CPU event pointer (raw disasm re-verified: 'or r3,r30,r30' at 0x82221CC8 kills it), the CPU-side simulated path (0x8223C7C0) shows the intended contract {store pool+0x2A94; KeSetEvent(pool+0x2BDC+cpu*0x38)}, but no code path that performs that KeSetEvent from the interrupt side has been found. Candidates not yet examined: kernel-side dispatcher semantics beyond VdSetGraphicsInterruptCallback, or a misidentified worker wait object. Do NOT guess a wake -- find the signal path first.

### Resolution (2026-07-22)
Occlusion queries: the title polls ZPD report blocks (sentinel 0xFFFFFEED) that only EVENT_WRITE_ZPD (0x5B) writes; the CP skipped that opcode. Implemented ZPD (zero samples) + EXT per the Xenia-documented contract, plus the stale swap-packet sequence guard. Presentation resumed; new frontier is frame rate (~0.2 fps from 30ms worker-wake polling), recorded above.

### Note (2026-07-22)
FRONTIER RESOLVED AS MISDIAGNOSED. The previous note's claim -- "the ISR provably discards the inner callback's returned per-CPU event pointer" and "no code path that performs that KeSetEvent from the interrupt side has been found" -- is FALSIFIED. Both halves were wrong, and both came from trusting a Ghidra decompile.

1. THE INNER CALLBACK DOES SIGNAL, ITSELF. Raw bytes at 0x8223B8A0 (decoded with the new tools/ppcdis.py, which reads the image with capstone and does not depend on Ghidra's listing state):
     lis r11,0x8200 / li r5,0 / li r4,1 / lwz r11,0x868(r11) / lwz r11,0(r11)
     stw r3,0x2A94(r11)            <- store the argument to pool+0x2A94
     lbz r10,0x10C(r13) / mulli r10,r10,0x38 / add r11,r10,r11
     addi r3,r11,0x2BDC            <- r3 = &percpu_event[cpu]
     b   0x82AC67E4                <- TAIL CALL, not a return
   ppc_func_mapping.cpp: { 0x82AC67E4, __imp__KeSetEvent }. So the callback IS
   {store pool+0x2A94; KeSetEvent(pool+0x2BDC+cpu*0x38, 1, 0)} -- exactly the contract the
   CPU-side path at 0x8223C7C0 shows. Ghidra rendered the tail branch as "return <that
   pointer>", which is what produced the phantom "discarded return value".

2. 0x82221CC8 "or r3,r30,r30" IS NOT A DISCARD. In raw disasm r30 there is r31+0x2A18 (loaded
   at 0x82221CC0), the argument to KeAcquireSpinLockAtRaisedIrql at 0x82AC66D4. The ISR takes
   that spinlock, clears its own CPU bit in *(*(ctx+0x2A14)), and releases
   (KeReleaseSpinLockFromRaisedIrql, 0x82AC66A4). Nothing is discarded.

3. THE SIGNAL PATH RUNS CORRECTLY IN OUR RUNTIME. Measured live: pool = 0x4015B080, cpu2 event
   = 0x4015DCCC, and the log shows INTERRUPT -> cpu 2 -> inner callback 0x8223B8A0 ->
   KeSetEvent(object 0x4015dccc) -> KeWait <- object 0x4015dccc signalled. There is no missing
   piece of the console's interrupt model here.

4. WHAT THE SLOT AT ctx+0x2A14+0x10 ACTUALLY IS: ctx+0x2A14 points at the SCRATCH write-back
   block (0x30B000). +0x10/+0x14 are SCRATCH_REG4/REG5. The stream writes REG4 = the callback
   to run, REG5 = its argument, REG0 = the CPU mask, then INTERRUPT, then REG4 = 0xBADF00D
   (the poison the ISR asserts on at 0x82221C7C). So the "inner callback" is chosen per
   submission by the command stream, and it ALTERNATES between two functions:
     0x8223B8A0  the per-CPU event signal above (arg = a retiring command-buffer pointer)
     0x8223E648  a VSYNC PACING callback -- and this one is where the time goes.

5. WHERE THE FRAME TIME ACTUALLY GOES (measured, not inferred). New frame-budget instrumentation
   in the CP splits its wall time. One representative frame:
     frame budget: 13122 ms total = 0 ms ring-empty (0 polls) + 12778 ms WAIT_REG_MEM
                   + 10 ms ISR (1567 interrupts)
       waits on 0x30b004: 784 times, 12778 ms
   So ~97% of every frame is the command processor blocked in WAIT_REG_MEM on 0x30B004
   (= SCRATCH_REG1's write-back slot), and ~0% is interrupt-wake latency. The 30 ms KeWait
   timeouts in the earlier note are an idle worker, not the bottleneck.
   NOTE: the previous session's waitStats "accumulate" lambda was defined and never called,
   which is why "waits on" never printed and the WAIT_REG_MEM cost stayed invisible. Fixed.

6. WHO WRITES 0x30B004 (the thing the CP waits for). Raw disasm of 0x8223E648..0x8223E6D4:
   arg r3 splits into hi = deadline in vblanks, lo = a raster-position percentage.
     r8 = hi - (vblankCount - lastAckVblank);  pool+0x3B20 += 1
     if r8 > 0                      -> pool+0x3B1C = r8   (still pending, NO ack)
     else if (*(0x7FC86530)*100 / (*(0x7FC86584)&0xFFF) + 1) > lo
                                    -> pool+0x3B1C = 1    (still pending, NO ack)
     else  *(*(pool+0x2A14)+4) = 0  <- THE ACK to 0x30B004; pool+0x3B18 = pool+0x3B14
   And the vblank arm of the ISR (0x82221D08..0x82221D3C) decrements pool+0x3B1C each vblank and
   writes the same ack when it hits zero. So 0x30B004 is released by a VBLANK-PACED countdown:
   each pacing request costs one or more 60 Hz vblank periods BY DESIGN. Observed args: 0x2000A
   (2 vblanks, 10%). Measured cost 8.1 ms per wait -- the mechanism is behaving correctly.

7. THE REAL DEFECT IS UPSTREAM: THE TITLE RE-SUBMITS THE SAME COMMAND BUFFERS. 784 pacing waits
   per frame is the symptom. Per-frame provenance counting shows every command-stream location
   executing exactly 38 times, and within one frame the same reg5 argument repeats
   (196x 0x2000a, 114x 0xc028f680, ...). New IB-provenance logging settles the cause:
     IB 0xf68a0 (3692 words) from ring dword 0xed6 / 0xe82 / 0xe2e / 0xd32 / 0xa3e / ...
   The same indirect buffer arrives from MANY DISTINCT ring slots, so the ring consumer is NOT
   lapping (that hypothesis is now ruled out twice) -- the TITLE is genuinely re-submitting the
   same buffers. One frame: 2352 IB submissions, each distinct IB submitted 44-88 times.

CONCLUSION / NEXT STEP: the frame rate is not a wake-path problem and needs no new interrupt
model. It is that the title's D3D layer re-kicks the same command buffers dozens of times per
frame, and each re-kick replays a vblank-paced INTERRUPT that costs ~8 ms. The next question is
why D3D re-kicks: find the guest loop that resubmits and what completion condition it waits on
that our GPU never satisfies (prime suspect: the served-ticket fence at 0x30A000 / the D3D lock
at 0x82221A68, since a fence that looks un-retired is exactly what makes a D3D layer resubmit).
Do NOT attack the vblank pacing -- it is faithful.

MEASURED, unchanged by this session (diagnostics only): intro ~30 fps, steady scene phase
~0.42 fps (60 frames in 144.03 s). No regression; no behavioural change was made.

NEW TOOL: tools/ppcdis.py -- capstone-based raw disassembly straight from the image, independent
of Ghidra project state. tools/ghidra_scripts/Disasm.py silently degrades to a byte dump when
Ghidra never rebuilt flow over the range (it did exactly that on 0x8223B8A0), so prefer
ppcdis.py as the cross-check of record.
