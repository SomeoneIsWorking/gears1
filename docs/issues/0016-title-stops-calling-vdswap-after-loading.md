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

### Note (2026-07-22)
RE-SUBMISSION QUESTION ANSWERED: NOT (a) tiled rendering. Tiling IS present and accounts for a
factor of 3, not the factor of 196. The remaining 196x is pathological, and on real hardware it
would be equally fatal -- so it is a defect (b/c), not faithful behaviour.

METHOD (new diagnostics in runtime/vd_null_gpu.cpp, all channel-gated, no behavioural change):
  - per-frame TYPE3 opcode census split by depth (ring level vs inside an indirect buffer),
    distinct-IB counts, and ring dwords written vs consumed  -> channel "gpu"
  - raw ring-level packet trace (dword index, header, dwords charged, wptr before/after)
    -> channel "ring"
  - unwrapped rptr/wptr accounting with a first-overshoot report. NOTE: the pre-existing
    "consumed + 1 > available" guard CANNOT detect lapping, because (wptr - rptr) is computed
    with masked arithmetic and a lapped read pointer reports an almost-full ring. The earlier
    "ring lapping ruled out" result was therefore a non-result from a detector that cannot fire.
    The unwrapped accounting is the detector that can, and it reports NO overshoot: rptr never
    passes wptr. Lapping is now genuinely ruled out, on evidence rather than on a blind test.

WHAT THE RING ACTUALLY CONTAINS (measured, one presented frame, scene phase). The ring-level
stream is one 42-dword unit repeated, each unit a COMPLETE frame submission:
    IB(11) IB(3255) IB(11) IB(23)
    SET_BIN_SELECT_LO 0x80000003 -> IB(3489) IB(29)
    SET_BIN_SELECT_LO 0xc        -> IB(3489) IB(29)
    SET_BIN_SELECT_LO 0xffffffff -> IB(11) IB(9589) IB(11) IB(3692)
    <VdSwap reservation packet>
12 IB packets * 3 dwords + 3 SET_BIN_SELECT_LO * 2 dwords = 42 dwords. Per presented frame:
197 such units, 2352 IB submissions, 8232 ring dwords written and 8232 consumed, 196 swap
packets of which 1 is accepted. Every published count divides by 196 exactly.

(a) TILED RENDERING IS REAL BUT IS NOT THE ANSWER. Predicated tiling is unambiguously in use:
the ring brackets scene replays with SET_BIN_SELECT_LO (0x62) and the indirect buffers carry
SET_BIN_MASK_LO/HI (0x60/0x61) in bulk (33516 / 14308 per presented frame). But the bin
selects take only THREE values (0x80000003, 0xc, 0xffffffff) and they are IDENTICAL in every
one of the 197 units. A tiling replay varies the bin selection per pass; a constant selection
repeated 197 times is not a tile loop. So tiling explains the 3 passes INSIDE a unit and
nothing else. Hypothesis (a) is refuted on the register values the title actually programs.

THE 196 IS "AS MANY AS FIT IN THE RING". Ring = 0x8000 bytes = 8192 dwords; unit = 42 dwords;
8192/42 = 195.0. The backlog was tracked unwrapped from CP start: it grows monotonically
(0 -> 2543 -> 6743 at packet 6000) and then PINS at 8163 = 8192 - 29 for the entire remainder
of the run. The ring is permanently full and the title is blocked on ring space. The repeat
count is not chosen by the title -- it is whatever the ring holds. That is the signature of an
UNBOUNDED resubmission loop whose only backstop is the ring filling up, not of any fixed
multi-pass scheme.

IT IS NOT FRESH RENDERING EITHER. Of the 197 units in one presented frame only 43 are distinct
recordings; the rest are repeats, and the repeats are INTERLEAVED, not consecutive (run-length
histogram: 181 runs of length 1, 8 of length 2). A small set of recorded command buffers is
being cycled and re-kicked round-robin. The ~86-96 "distinct IBs at 44-116 submissions each"
from the previous note is this same fact seen per-buffer.

CORRECTION TO THE PREVIOUS NOTE: "the same IB arrives from many DISTINCT ring slots, so the
title is genuinely re-submitting" was the right conclusion for the wrong reason -- distinct
ring slots are also what lapping produces. The correct proof is the write pointer: wptr
advances 8232 dwords per presented frame, and wptr is the title's own plain `stw` at
0x82221424 (verified with tools/ppcdis.py: `lis r11,0x7fc8 / stw r30,0x714(r11)`, big-endian,
so ReadGuest32 reads it correctly). The title really does write 8232 dwords per present.

STALE SWAP SEQUENCES ARE A CLUE, NOT NOISE. The 195 rejected swap packets carry sequences in a
narrow recent band (592-598 while current is 600), not a spread over 196 past frames. So the
re-kicked buffers are ones VdSwap stamped within the last ~8 presents: D3D is recycling a
small pool and re-submitting recent recordings, consistent with a retry loop over the last few
frames' buffers rather than a backlog of ancient ones.

PHASE-SPECIFIC: in the intro (~30 fps) a presented frame is a single unit of ~2-6 IBs. The
197x replication appears only when the 3D scene phase starts, i.e. together with the tiled
3-pass structure.

WHAT REMAINS UNESTABLISHED (do not guess it): the guest-side condition that drives the retry.
Candidates named but NOT tested this session: the served-ticket fence at 0x30A000 and the D3D
adaptive lock at 0x82221A68. The frame-latency throttle a title normally uses (1-3 frames in
flight) is provably not working here -- 196 frames are in flight and the only thing that stops
the title is ring exhaustion -- so whatever implements that throttle is the place to look. The
next step is to find the guest loop that re-kicks and the completion condition it re-tests,
NOT to change anything in the command processor.

MEASURED, unchanged by this session (diagnostics only): intro 30.27 fps, scene phase 0.45 fps
(600 frames, last 60 in 134.21 s). No regression, no behavioural change, nothing committed.

### Note (2026-07-22)
PHASE 1 (contract) -- partial, with the amplification located but its producer not yet named.

NEW CAPABILITY, and it had to be built first: the override mechanism this port
assumed it had DID NOT WORK. XenonRecomp emits
    __attribute__((alias("__imp__sub_X"))) PPC_WEAK_FUNC(sub_X);
so that a strong sub_X wins at link time. Clang folds a call to an alias into a
call to its ALIASEE inside the defining translation unit, so every intra-TU call
site becomes `call __imp__sub_X` and no link-time symbol can intercept it.
Verified in the binary: __imp__sub_82221980 calls __imp__sub_822218C0 directly.
Measured: nine strong overrides installed, entered ZERO times over a whole movie
phase. Since the D3D layer is one dense cluster, most of its calls are intra-TU
-- exactly the set an HLE seam must intercept. Two further traps found on the
way: (1) the recomp symbol sub_X is C++-mangled, not extern "C", so an
extern "C" override silently defines an unrelated symbol and links cleanly;
(2) scratch/ppc held 191/192 as STALE leftovers of an earlier recompiler run,
353 functions defined twice (byte-identical), and the link only survived while
lazy archive extraction happened not to pull both members -- adding any
__imp__ reference broke it with 256 multiple-definition errors.
Fixes: tools/dedupe_recomp.py (removes byte-identical duplicates and stale TUs;
--check is clean now) and tools/prepare_overrides.py (strips the alias line for
declared overrides so intra-TU calls become external references). New TU
runtime/hle_d3d.cpp holds the overrides + a call census with call-site
provenance; runtime/hle_d3d.h; wired into VdSwap so the census prints per frame.
Movie phase still 30.05 fps with the probes in, so no regression.

MEASURED CONTRACT, scene phase, per PRESENTED frame (deltas between consecutive
VdSwap censuses, all exact and repeatable):
    sub_822218C0  D3D submit entry            +8      <- the API submits 8 times
    sub_82221980  flush                       +5
    sub_82221A68  ticket fence wait           +4  (of which +1 is the throttle)
    sub_8223B5E0  worker replays a CPU list   +392
    sub_8223B200  CPU command-list interpret  +392
    sub_822212D8  RING KICK                   +2352  = 392 x 6
Provenance of the 2352 kicks: 100% from 0x8223B304, i.e. from inside the CPU
command-list interpreter run by the D3D worker thread. The direct path
(0x82221970, inside sub_82221980) contributes ZERO in the scene phase -- its
counter is frozen at 1154 for the whole run. The 2352 exactly reproduces the
previously published "2352 IB submissions per presented frame", so the
amplification is entirely between the 8 API submissions and the 392 worker
replays.

THE THROTTLE IS FOUND, IS RUNNING, AND IS NOT THE PROBLEM. Raw disasm of
0x8223EEF0..0x8223EF38 (the present wrapper):
    r30 = *(dev+0x2A1C)                 ; ticket BEFORE this frame's flush
    sub_82221980(dev)                   ; flush, emits the fence, bumps 0x2A1C
    sub_8223E3E0(dev,0,0)               ; Present -> VdSwap
    sub_82221050(dev, *(dev+0x34B4)>>2) ; next command buffer
    if (*(dev+0x34D0)) sub_82221A68(dev, *(dev+0x34D0), 3)   ; WAIT for the
                                        ;   PREVIOUS present's ticket
    *(dev+0x34D0) = r30                 ; remember this frame's ticket
That is the ~2-frames-in-flight latency throttle, and the census shows it
running once per frame (+1 per frame at call site 0x8223EF30). The same wait
appears in the command-buffer allocator at 0x822211FC. So "the frame-latency
throttle is not working" is FALSE as stated: it works, and it is not what the
ring exhaustion comes from. The exhaustion is submissions WITHIN one frame.

A SECOND, INDEPENDENT FRAMES-IN-FLIGHT RING EXISTS and was mistaken for nothing:
0x8223E928..0x8223EB68. dev+0x4E54 = produced, dev+0x4E50 = retired, limit
(produced - retired) >= 6, slots at *(dev+0x2A10) + 4*(16 + (idx & 7)), zeroed
by the CPU and filled by an EVENT_WRITE_SHD (0xC0025800 / initiator 0x80000003 /
value 0xDEADBEEF). The retire scan is at 0x82237070 (lwbrx of three slots,
advances 0x4E50 by 2). It only SKIPS the fence when full; it never waits. Our CP
does service that EVENT_WRITE_SHD, so this ring is not stuck.

RULED OUT, on evidence: the block at 0x82221A00..0x82221A48 (test *(0x82BED120),
wait for ticket-2) is NOT a throttle. That global is set at 0x8223AA5C from
VdIsHSIOTrainingSucceeded() == FALSE, and the same arm prints "D3D: GPU
initialization (HSIO training) has failed so no graphics will render." We
return TRUE, so the global is 0 and the block is correctly dormant. Do not
"enable" it.

WHERE THE AMPLIFICATION ACTUALLY IS, and what is still unknown. The worker
thread loop is 0x8223B7E8: KeWaitForSingleObject(worker+0x20, 30 ms timeout);
on STATUS_TIMEOUT it runs the present pump (0x8223E6D8 + 0x8223E860); on signal
it calls sub_8223B5E0, which takes the queued list from context+0x58 (clearing
it) and interprets it. Instrumented the queue head at every replay:
    per scene frame: 296 replays take a list address NEVER seen before,
                      96 replays repeat the previous address,
                       0 replays find an empty queue.
So the queue is genuinely refilled ~296 times per frame while the D3D API
submits 8 times. The producer of those enqueues is NOT sub_822218C0 and has NOT
been identified. That is the open question, and it is the whole remaining
distance to the answer -- do not guess it. The next step is to instrument writes
to context+0x58 (or override sub_82220B40, the recorder) and capture the caller.

PHASE 2 (native override) NOT ATTEMPTED. Overriding the submission layer before
knowing who enqueues 296 lists per frame would be exactly the "patch the middle
of a state machine you do not understand" the task forbids. The mechanism to do
it now exists and is proven (nine functions overridden and measured), which is
the part that was actually missing.

NOT ESTABLISHED / regressions: no frame-rate measurement of the scene phase was
taken in these runs (the instrumented runs were stopped in the 588-600 frame
window before a 60-frame fps sample completed); movie phase measured unchanged
at 30.05 fps. Nothing in the command processor was changed. Nothing committed.

### Note (2026-07-22)
PRODUCER OF THE WORKER QUEUE IDENTIFIED, ON EVIDENCE, AND THE RE-ENQUEUEING IS PATHOLOGICAL.

METHOD. Static search cannot find the writer: the ONLY `stw rX,0x58(rY)` in the D3D range is
the interpreter's own clear at 0x8223B670, so the enqueue reaches the field through a
different base pointer. New capability instead (runtime/hle_d3d.cpp, GEARS_WATCH_QUEUE=1): an
mprotect + SIGSEGV + single-step write watchpoint on the guest field, recording the faulting
host RIP and re-arming per presented frame. Two writers, and only two:
    __imp__sub_8223B5E0   the interpreter clearing the queue it just took
    __imp__sub_8223B8A0   THE GPU INTERRUPT CALLBACK          <- the producer
(Trap worth recording: dladdr's dli_fbase is 0x400000 for this non-PIE binary, so RIP is
already the addr2line VMA. Subtracting the base first symbolised into an unrelated function
and cost a wrong conclusion. Feed addr2line the raw RIP.)

WHAT THE PRODUCER IS. <interpreter ctx> = pool+0x2A3C, so ctx+0x58 IS pool+0x2A94, the slot
the interrupt callback at 0x8223B8A0 stores its argument into before KeSetEvent'ing the
per-CPU event. Nothing new has to be discovered about that callback -- it is the same one
already documented in this entry. The submission's SCRATCH_REG5 carries a CPU command-list
pointer; the CP's INTERRUPT packet runs the callback; the callback enqueues that list and
wakes the D3D worker, which replays it (sub_8223B5E0 -> sub_8223B200) and whose replays emit
ring kicks. The loop is therefore CLOSED: ring -> INTERRUPT -> enqueue -> worker replay ->
ring. 100% of the 2352 kicks/frame already came from the worker (0x8223B304), and now 100% of
the worker's wakeups are shown to come from the ring. Nothing outside the loop drives it.

IT IS A POSITIVE-FEEDBACK LOOP WITH GAIN > 1 (measured per presented frame, scene phase):
    replays          524      each emitting exactly 6 ring kicks   -> 3144 kicks
    enqueues         786      = 1.5 enqueues per replay            <- GAIN 1.5
    of those, 312 re-enqueued the very list being replayed, and 262 OVERWROTE a list the
    worker had not consumed yet (the queue is a single slot; the overwritten continuation is
    silently dropped -- and if a resume pointer is set, sub_8223B5E0 at 0x8223B668 discards
    the enqueued head outright and runs the resume instead).
    replays split exactly 50/50: 262 suspended (left a resume pointer at ctx+0x50) and 262 ran
    to completion. Another frame: 750 replays / 1124 enqueues / 4496 kicks, same ratios.
Because each replay produces more than one enqueue, the population of pending replays grows
until the only backstop -- the ring filling up -- caps it. That is the 196x already published,
now explained rather than merely observed.

THE REPLAYS MAKE NO PROGRESS. Per frame the 524-750 replays cover only 45-64 distinct
(list, resume) start points, and the SAME point recurs up to 68 times in one frame. Crucially
the recurring points include resume == 0, i.e. the same recorded list is re-run FROM THE START
dozens of times per frame (e.g. `list 0xc021d400 resume 0x0 x48`). A list re-run from the start
re-emits all of its ring packets, including the INTERRUPT that re-enqueues it. Only ~8-12
distinct list pointers exist in a frame. This is not an engine legitimately recording many
command lists: it is a small set of lists cycling.

ANSWER TO THE FRAMED QUESTION: pathological, not legitimate. The coroutine design itself is
legitimate (a list suspends on a GPU-sync token and the ISR resumes it via ctx+0x50 -- 262
suspend / 262 complete per frame is that mechanism working), but the loop gain of 1.5 and the
restart-from-zero repeats are not. On hardware a replay must not, on average, cause more than
one further enqueue.

WHAT REMAINS UNESTABLISHED, and it is the next step. WHY a completed list is re-enqueued at
resume 0. The nomination sites that install callback 0x8223B8A0 with a list argument are only
three (0x82236A80 -> sub_82221E68, 0x8223BA94 -> sub_82221B40 inside sub_8223BA18, and
0x8223C2FC in the CPU-side dispatcher), and sub_8223BA18 is called ~30 times in a WHOLE RUN --
so ~30 nominations produce ~800 enqueues per frame. The multiplier is entirely that our CP
re-executes INTERRUPT packets sitting inside indirect buffers the worker keeps re-kicking.
Two candidate root causes, NEITHER TESTED:
  (a) predication. The IBs carry SET_BIN_MASK_LO/HI in bulk and the ring brackets them with
      SET_BIN_SELECT_LO; if our CP ignores predication it executes packets (including the
      INTERRUPT) that hardware would skip. This is the strongest candidate and is cheap to
      test: count INTERRUPT packets executed inside predicated-off regions.
  (b) a stale-packet defect of the same shape as the VdSwap one fixed earlier in this entry:
      the INTERRUPT + SCRATCH_REG4/REG5 triple lives in reused guest command-buffer memory and
      is re-executed from a buffer that was not re-recorded.
Do NOT "fix" this by suppressing enqueues or by rate-limiting the worker; that would hide the
loop, not break it. Find which packet the hardware would not have executed.

CODE: runtime/hle_d3d.cpp only -- watchpoint (opt-in, off by default), enqueue census on a new
override of sub_8223B8A0, ring-kick IB-size census, and a (list,resume) replay-progress
census. All channel-gated on "hle", all pure instrumentation; nothing in the command processor
or in guest behaviour was changed. tools/prepare_overrides.py now also recognises hand-written
PPC_FUNC(sub_X) overrides, not just the GEARS_HLE_TRACE macro -- sub_8223B8A0's alias was still
in place and its override would have been silently bypassed by intra-TU call sites.

MEASURED: intro 29.70-30.29 fps, scene phase 0.41 fps (600 frames, last 60 in 145.32 s) with
all probes installed -- unchanged from the 0.42-0.45 fps of record, so no regression. No fix
was attempted; nothing committed.

### Note (2026-07-22)
HYPOTHESIS CONFIRMED AND FIXED: our command processor ignored PM4 predication.
Scene phase 0.42 fps -> 29.9 fps sustained (71x). The positive-feedback loop is gone.

CONTRACT (from extern/xenia, src/xenia/gpu/pm4_command_processor_implement.h and
command_processor.h, cross-checked against what the title actually programs):
  - The PFP holds two 64-bit registers, BIN_MASK and BIN_SELECT, BOTH RESET TO
    ALL-ONES ("everything passes"), so a title that never programs them is
    unaffected.
  - Written by TYPE3 opcodes SET_BIN_MASK 0x50 / SET_BIN_SELECT 0x51 (two data
    words, HI first then LO) and by the half-register forms SET_BIN_MASK_LO 0x60,
    _HI 0x61, SET_BIN_SELECT_LO 0x62, _HI 0x63 (one word each).
  - BIT 0 OF A TYPE3 PACKET HEADER IS THE PREDICATE BIT. When set, the packet
    executes only if (bin_select & bin_mask) != 0; otherwise the PFP consumes its
    count and skips it. Only TYPE3 packets are affected; TYPE0/1 are not.
  Guest usage matches exactly: the ring brackets scene replays with
  SET_BIN_SELECT_LO (three values 0x80000003, 0xc, 0xffffffff) and the indirect
  buffers carry SET_BIN_MASK_LO/HI in bulk -- classic Xenos predicated tiling,
  where ONE recorded command buffer is replayed per EDRAM tile with the packets
  that do not belong to that tile masked off.

INSTRUMENTED BEFORE CHANGING BEHAVIOUR (predication census in the CP, per
presented frame, scene phase, measurement-only build):
    packets executed while (select & mask) == 0 : 32340
    predicated packets: DRAW_INDX 21285 (skip 0)    SET_CONSTANT 14112 (skip 7056)
                        DRAW_INDX_2 4508 (skip 980) WAIT_REG_MEM  1568 (skip 392)
                        EVENT_WRITE 1568 (skip 784) EVENT_WRITE_EXT 1568 (skip 784)
                        INTERRUPT    784 (skip 196)
So 196 of the 784 INTERRUPT packets per frame were being executed that hardware
would have skipped -- and 196 is exactly the replication factor of record. Note
the census UNDERSTATES the effect: it is measured inside the runaway loop, so
each skipped INTERRUPT also removes the enqueue -> replay -> 6 ring kicks it
would have caused. The prediction was therefore "loop gain drops below 1", not
"25% fewer interrupts", and that is what happened.

FIX (runtime/vd_null_gpu.cpp, ~40 lines): CommandProcessor now keeps binMask /
binSelect (both initialised 0xFFFFFFFF), updates them in HandleType3 for opcodes
0x50/0x51/0x60..0x63, and in ExecutePacket skips any TYPE3 packet whose header
has bit 0 set while (binSelect & binMask) == 0, consuming its count. Nothing else
changed; no suppression of enqueues, no rate-limiting, no dropped interrupts --
the loop was broken by executing the stream correctly, not by damping it.

MEASURED AFTER, long runs, same logging configuration as the before-run:
    scene phase   0.42 fps  ->  29.89 fps, sustained from frame 660 to frame 4740
                                (samples at 660/1380/2100/2820/3540/4260: 29.89,
                                 29.89, 29.88, 28.80, 27.78, 29.83)
    intro/movie   30.0 fps   ->  30.18 fps (unchanged, as expected: the bin
                                registers are all-ones there)
    per frame     2352 IB submissions, 8232 ring dwords  ->  12 IB submissions,
                  42 ring dwords written and consumed. That is EXACTLY the single
                  42-dword unit the ring-content analysis said one frame should
                  be, so the 196x replication is fully accounted for.
    per frame     784 INTERRUPTs -> 4;  784 pacing waits (12.8 s) -> 3 interrupts
                  and 29 ms of WAIT_REG_MEM in a 33 ms frame (vblank pacing,
                  faithful, now the only cost).
    zero "GPU is hung", zero "WAIT_REG_MEM stuck".

WHY THE EARLIER ANALYSIS POINTED HERE CORRECTLY BUT READ ONE FACT WRONG: the
previous note refuted tiling as the explanation because "the bin selects take
only three values and are identical in every one of the 197 units". That is true
and is still the right observation -- but it refutes tiling as an explanation of
the 197 REPEATS, not tiling itself. The tiling was real, and the CP's failure to
honour it is what made each of the 3 masked passes execute in full, INTERRUPT
included, which is what generated the repeats in the first place.

NEW FRONTIER, NOT PART OF THIS DEFECT: the run now reaches frame ~4740 (~160 s of
real gameplay) and then dies with the physical heap exhausted -- 90x
"[heap:error] out of guest heap: wanted 0x10000 bytes, 0x0 left" against the
512 MiB physical heap, after which the title prints "ERR[D3D]: Unanticipated
CPU_INTERRUPT. Sign of a corrupt command buffer?" and the process takes SIGSEGV.
The D3D complaint and the crash are DOWNSTREAM of the allocation failures (they
appear ~8000 log lines after the first one), not of predication. This is a guest
heap sizing/leak question that was simply unreachable before, because the title
never got this far. Do not conflate it with this entry.

CANDIDATE (b) FROM THE PREVIOUS NOTE -- the stale-packet defect -- was NOT needed
and is not currently supported by evidence: with predication honoured the per
frame counts collapse to the exact recorded unit, leaving no unexplained
re-execution.
