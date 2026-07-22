---
id: 12
title: title reports the GPU is hung
status: resolved
symptom: guest DbgPrint: 'Breaking into the debugger. The GPU is hung and can't be recovered without doing a cold boot' -- reached after startup completes, with exactly one VdSwap
tags: gpu,graphics
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
Reached only after async Xam completion was fixed; before that the title stalled earlier and never got here. The diagnosis is the title's own and it is CORRECT -- the runtime's GPU is an explicit null.

What exists: VdInitializeRingBuffer records the ring, RetireRingBuffer reports the whole ring as consumed by writing the write pointer to the read-pointer write-back address, and a host thread drives the graphics interrupt callback at 60 Hz (source 0 = vblank).

Concrete gap found: VdSetSystemCommandBufferGpuIdentifierAddress stores the address in g_systemCommandBufferGpuIdentifier and NOTHING EVER WRITES TO IT. On hardware this is a fence the GPU updates so the title can see which command buffer has been consumed. A title polling it sees no progress, which is consistent with declaring the GPU hung.

DELIBERATELY NOT GUESSED: what value belongs there. The identifier echoes back a submission id the title supplies, and inventing a monotonically increasing number would be a value chosen to make the symptom disappear rather than one derived from the protocol -- the exact shape of bandaid this project bans. Determine what the title writes into the system command buffer first, then echo that.

Note also that the interrupt callback is only ever raised with source 0 (vblank), never a command-completion source. Whether the title needs a completion source is unverified and should be checked rather than assumed.

The real fix is a command processor that parses the ring buffer, which is a substantial subsystem and not a stub.

### Note (2026-07-22)
SYSTEMATIC INVESTIGATION -- evidence, not assumption.

Established the failure signal. The 'GPU is hung' text at 0x820BDA98 is referenced from 0x8222FB84, and the companion string at 0x820BE050 from 0x8222FE08. Both sit inside sub_8222FD78, which is the ESCALATION PATH OF THE ADAPTIVE LOCK already understood earlier: sub_8222F460 calls it once the tick delta passes 5000. So the thing that lock waits on is GPU progress, and the two investigations are one.

Established the contract. In sub_82221A68 the wait is modular sequence arithmetic: spin while (next - ticket) < (next - served), where next = *(pool+0x2A1C) and served = **(pool+0x2A10). Measured live at the hang with a breakpoint on sub_8222FD78: pool=0x4015B080, served counter address = 0xA030A000 holding 1, ticket next = 9. So the GPU must advance the word at 0xA030A000 from 1 to at least 9.

Three hypotheses tested and RULED OUT:
1. 'It is one of the addresses a Vd* call gave us.' No. Those are 0xA030A008 (system command buffer GPU identifier) and 0x0030A03C (ring read-pointer write-back). The polled word 0xA030A000 is 8 bytes before one and 0x3C before the other -- same block, different words. It is never handed to the runtime by any registration.
2. 'A TYPE3 packet in the command stream names it.' No. A read-only PM4 walk over the ring and the 6-8 indirect buffers it follows found no data word carrying the address (compared on the physical offset, so window aliasing cannot hide it).
3. 'A TYPE0 register write names it.' No. The walker was extended to inspect register payloads as well -- 82 walks, no hit.

Also confirmed by watchpoint on all four aliases: exactly ONE write to 0xA030A000 in a whole run, from guest code during startup (_xstart -> sub_82218F98 -> sub_824A4D48 -> sub_8223A840 -> sub_82221EC0) initialising it to 1. Nothing writes it afterwards, so on hardware the GPU is the only other writer.

Remaining candidate under test: the guest programs the address into a Xenos register directly through the MMIO window at 0x7FC00000 using byte-reversed stores, bypassing the ring buffer. That window is committed as inert memory, so such a write lands silently and the runtime never sees it.

NOT DONE, deliberately: no value has been written into 0xA030A000 to make the symptom go away. Until the mechanism that names the address is found, any value would be chosen to silence the error rather than derived from the protocol.

### Note (2026-07-22)
FOURTH HYPOTHESIS ALSO RULED OUT. Scanned the whole device MMIO window (0x7FC00000..0x80000000, both byte orders, compared on the physical offset) at the moment of the hang: the fence address appears nowhere in it. So the guest does not program it into a Xenos register either.

All four candidate mechanisms are now eliminated:
  1. an address the runtime was handed by a Vd* registration -- no
  2. a TYPE3 packet data word -- no
  3. a TYPE0 register payload -- no
  4. a Xenos register programmed directly through MMIO -- no

That the address appears in NONE of the places the GPU could learn it from is the
key result, and it redirects the investigation: the word is probably not written
by the GPU at all. It is far more likely written by the TITLE'S OWN graphics
interrupt handler, which computes the destination from its own structures -- which
is exactly why the address never appears in any stream or register.

The runtime registers that handler via VdSetGraphicsInterruptCallback (observed at
0x82221C60) and a host thread invokes it at 60 Hz, but ALWAYS with source 0
(vblank) and never with a command-completion source. A handler that only ever sees
vblank would have nothing to retire, and would leave the counter where it is --
matching the observation exactly.

Note the initialising write came from sub_82221EC0, which is in the same 0x82221xxx
neighbourhood as the interrupt callback at 0x82221C60 and the lock at 0x82221A68.
That whole cluster is one subsystem and is where the answer is.

NEXT MEASUREMENTS, in order:
  a. Confirm the interrupt callback actually executes when the host thread invokes
     it -- do not assume it does.
  b. Decompile the callback at 0x82221C60 and find which source values it acts on
     and what it reads to decide what completed.
  c. Only then decide what the runtime must supply: probably a completion source
     plus whatever GPU state the handler consults.

### Note (2026-07-22)
ISR CONFIRMED AND PARTIALLY ADDRESSED.

Confirmed by raw disassembly (not decompiler output -- the decompile's prologue runs through a stubbed save/restore helper and its local for the source argument is an artifact):
  82221C6C  or r31,r4,r4       ; r31 = interrupt CONTEXT (arg 2)
  82221C70  cmplwi cr6,r3,0x1  ; source (arg 1) == 1 ?
  82221C74  bne cr6,0x82221cf0 ; else -> vblank path
So source 1 is the command-complete path and source 0 the vblank path, as read.

Measured: the ISR is invoked 8350 times in one run and EVERY call is source 0. Source 1 is never delivered by the runtime. That is gap A and it is the path that retires work.

Gap B, addressed: the vblank path is additionally gated on MMIO 0x7FC86544 bit 0. A scan of the whole image finds that address accessed at exactly ONE site -- the load at 0x82221CFC inside this ISR -- and never written, so it is a status bit the GPU owns and the title only samples. The runtime is the GPU and does deliver vblank, so reporting the bit clear described a machine that cannot exist. It is now set when the device window is committed, and verified to still read 1 at the hang.

UNVERIFIED, and must not be recorded as either working or not: whether the vblank path now actually executes. The check made was to read pool+0x3B14 using the pool from sub_8222FD78's r3, but the ISR indexes its structures off r4, the interrupt CONTEXT, which is not known to be the same object. The counter reading 0 therefore proves nothing. Redo this by capturing r4 at ISR entry and reading r4+0x3B14 across two samples.

Setting the bit did NOT clear the hang on its own, which is expected: gap A remains, and command completion is what advances the served counter.

NEXT: deliver source 1. Requires knowing when a command buffer completes, which under instant retirement means 'on submission' -- so first find what the title does to submit (a write to the ring write pointer register is the likely candidate, and would need that MMIO write to be observable rather than landing in inert memory).

### Note (2026-07-22)
RESOLVED -- the GPU-hang stall is cleared. Full protocol found and implemented.

SUBMISSION (question 1): the title kicks the GPU by storing the ring write index (dword units) to CP_RB_WPTR at MMIO 0x7FC80714 -- plain stw at 0x82221424, the only such store in the image (found by tools/scan_mmio_stores.py, a static scan for device-window lis bases). The kick function appends a TYPE3 INDIRECT_BUFFER packet to the ring, stores the new write index at pool+0x2A44, sync, then the MMIO store. Since the device window is committed memory, the store lands there; the runtime's command processor thread simply polls that word.

SERVED COUNTER (question 2): advanced by the GPU itself via TYPE3 EVENT_WRITE_SHD (opcode 0x58). Function 0x82221640 builds each submission's retirement template: EVENT_WRITE_SHD with address word = physical(*(pool+0x2A10))|2 (= 0x0030A002) and value = the ticket from pool+0x2A1C; next += 2 per submission (tickets 3,5,7,9... -- served init next-2=1 matches). WHY ALL FOUR EARLIER SCANS MISSED IT: packet address words carry the Xenos endian mode in their low 2 bits, so the word is 0x0030A002, and pm4_trace compared PhysicalOffset equality INCLUDING those bits (0x30A002 != 0x30A000). Second scan hole: VdInitializeRingBuffer's size_log2 counts QUADWORDS (bytes = 8<<log2; verified live: guest ring mask pool+0x34A8 = 0x1FFF dwords = 0x8000 bytes with log2=12), while the runtime assumed bytes -- every prior walk covered only 1/8 of the ring.

INTERRUPT PROTOCOL: scratch writeback. The title programs SCRATCH_ADDR (reg 0x1DD) = physical of the 0x20-byte block at *(pool+0x2A14) (= 0x30B000) and SCRATCH_UMSK (reg 0x1DC) = 0x20033: a TYPE0 write to SCRATCH_REGn (0x578+n) is copied by the GPU to SCRATCH_ADDR+4n for masked n (bits 0,1,4,5 = exactly the ISR's fields +0,+4,+0x10,+0x14). Per submission the stream writes REG4=callback pointer (0x8223E648, frame pacing), REG5=arg, REG0=CPU pending mask (4), then TYPE3 INTERRUPT opcode 0x54 (NOT 0x40 -- that's later Adreno) with data=cpu mask raises source 1; the ISR (0x82221C60) calls *(0x30B010) with *(0x30B014), clears its CPU bit in 0x30B000, and the stream WAIT_REG_MEMs (0x3C) on 0x30B000==0, restores REG4=0xBADF00D sentinel, waits 0x30B004==0 (cleared by the vblank countdown path / pacing callback).

VBLANK GATE (question 3): the earlier flaw was real and material. 0x7FC86544 is read with plain lwz (big-endian), not lwbrx; the previous fix stored the bit little-endian, so the guest read 0x01000000, bit0 clear, and the vblank path NEVER executed. Now stored guest byte order. VERIFIED live: pool+0x3B14 advances 0x0C06->0x0C48->0x0C89 over 2s (~60 Hz).

ALSO REQUIRED: the pacing callback 0x8223E648 divides by *(0x7FC86584)&0xFFF (vertical total; it computes scanline*100/total from *(0x7FC86530)). Inert zero = SIGFPE (reproduced, coredump in sub_8223E648). Now reports 750 (CEA-861 vertical total for the 1280x720p60 mode the runtime already reports), scanline register stays 0 (always in vblank, consistent with the status bit).

IMPLEMENTATION: runtime/vd_null_gpu.cpp now runs a command processor thread: polls CP_RB_WPTR, walks the ring, executes TYPE0 (register file + scratch writeback), INDIRECT_BUFFER, EVENT_WRITE_SHD, MEM_WRITE, WAIT_REG_MEM (memory+register spaces, all compare functions), INTERRUPT (dispatches source 1 per CPU in mask, stamping KPCR+0x10C so the ISR clears the right pending bit); draws are skipped by count. Instant-retirement RetireRingBuffer removed. pm4_trace opcode table corrected (0x10 NOP, 0x48 ME_INIT, 0x54 INTERRUPT, 0x58 EVENT_WRITE_SHD).

VERIFIED RESULT: 150s run, zero 'GPU is hung' prints (was 100% reproducible); served counter advanced 3,5,7,9 by EVENT_WRITE_SHD; 4 submissions retired; title proceeds -- spawns guest thread 8 (entry 0x82941df0), re-queries video mode, issues XMsg app 0xfa/0xfb/0xfc calls that the runtime rejects as 'no such service'. NEXT FRONTIER: whatever the title waits on after D3D device creation -- likely those XMsg services and/or further VdSwap-driven present flow (only 1 VdSwap so far).

### Resolution (2026-07-22)
The runtime never executed the driver protocol packets: submission arrives as a CP_RB_WPTR MMIO store (0x7FC80714) that landed silently in the inert device window, and retirement is EVENT_WRITE_SHD writing the ticket to 0x30A000 plus scratch-writeback + INTERRUPT(0x54) driving the title's ISR -- none of which existed. Implemented a minimal command processor in runtime/vd_null_gpu.cpp; verified the hang is gone.

### Note (2026-07-22)
ADDENDUM (scene phase): the 'GPU is hung' symptom returned during real scene rendering with the movie-phase protocol still correct. That recurrence was a DIFFERENT root cause -- VdSwap not filling the title's reserved 64-dword swap block in the command buffer, desyncing the CP walk past frame fences. Full analysis and fix recorded in entry #14. The command processor now also logs fence provenance (buffer+offset) which was decisive.
