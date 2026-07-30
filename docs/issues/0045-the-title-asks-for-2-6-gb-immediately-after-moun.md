---
id: 45
title: Checkpoint restore calls through a freed object (was: the title asks for 2.6 GB after mounting save content)
status: open
symptom: deterministic SIGSEGV at ~1560 frames on the checkpoint restore path, calling virtual slot 13 of a freed pool block; earlier and now superseded: a 2.6 GB allocation after the save content mount
tags: saves,content,memory,port
created: 2026-07-29
updated: 2026-07-30
---

> ## STATE AS OF THE LATEST NOTE — READ THIS FIRST
>
> **The 2.6 GB allocation this entry is named after NO LONGER HAPPENS, and it was
> never the defect.** It was a garbage FString length read through a null pointer
> into low guest memory (which is mapped, because the physical alias window
> starts at guest address 0). When the allocation pattern shifted, that garbage
> became zeros and the symptom vanished on its own — without the cause moving.
>
> **What is true now, all measured:**
> - The title reaches the campaign, writes a save (`created 'save:\Pla'`), and
>   then takes the checkpoint RESTORE path for the first time.
> - It dies deterministically at ~1560 frames in `sub_824961D0`, calling virtual
>   slot 13 on the object cached at `holder+1376`.
> - From the core: `holder = 0x41c94e00`, `cache +1376 = 0x42babc40`,
>   `guard +1396 = 00000000`. The guard is the "use the cache" branch, and the
>   cached object is a **freed pool block** — head of a 214-node free list whose
>   descriptor holds it. Its "vtable" is that list's next pointer.
> - The block is released by the guest's OWN pool realloc (`sub_822151F8`, free
>   site `0x822153E4`), measured live with `GEARS_WATCH_FREE`.
> - The `LoadChapter` op DOES run and DOES fill the carrier with 385 real bytes.
>   The carrier was never the problem.
>
> **So: a stale cache that nothing invalidates — and it is now measured end to
> end.** A probe over the pool's free, checking every live holder's cache, finds
> the object is released WHILE STILL CACHED **156 times in a single run**, and
> the last one is the crash:
>
> ```
> FREEING 0x42babc40 WHILE IT IS STILL CACHED at holder 0x41c94e00+1376
>   (occurrence 156)
> ```
>
> — exactly the object and holder gdb reports at the fault. The title survives
> the first 155 because the freed block usually still *looks* like the old
> object; it dies when the block has been reused and its first word is no longer
> a vtable.
>
> `sub_824961D0` takes the holder in **r3** (`mr r24,r3`), and its prologue is
> `lwz r11,1376(r24) ; cmplwi 0 ; bne` — the cache is used whenever it is merely
> NON-NULL. There is no validity check, and nothing nulls the field on free.
>
> The open question is now narrow: is that free legitimate? The pool is guest
> code, so it behaves as on console GIVEN THE SAME SIZES — which points at a size
> we supply rather than at the allocator.
>
> **Everything below is the investigation trail, oldest first, and several early
> notes are FALSE.** They are kept because the dead ends are worth knowing, not
> because they are true. In particular, disregard: "the title does not treat the
> refusal as fatal" (it segfaults), "this is why no save file appears" (the save
> is written), "the archive array is empty" (a capped probe saw 4 of 78,278
> calls; at the crash it holds a corrupt descriptor), and the device-selector
> fix claim (retracted — it changed memory luck, not the cause).



Appeared the moment saves started working, and it is on a path that was
previously unreachable: XamContentCreateEx used to return "device not
connected", so the title never got as far as this allocation.

TWO EXPLANATIONS TRIED, THE FIRST ONE WRONG.

I first assumed I had caused it by reporting the host's real disk capacity in
XamContentGetDeviceData -- a PC's terabytes where a console would report tens of
gigabytes -- and that the title sized a buffer from it. Capping the device to
512 MB (the console's own Memory Unit) did NOT remove the allocation. So the
size does not come from the device data, and that explanation is dead.

The cap stays anyway, on its own merits: reporting a number no console could
return is asking a title to handle a case it was never written for, and 512 MB
is far more than a save needs. But it is not a fix for this.

WHAT IS ACTUALLY KNOWN: the request is for exactly 0x9f000000 bytes
(2,667,577,344), type 0x20001000, protect 0x4, immediately after the content
mount. The guest heap has ~460 MB free and refuses. The title does not treat the
refusal as fatal -- the run continues at 29.4 fps with rendering and audio
normal -- but a subsystem that just asked for 2.6 GB and was told no is unlikely
to be in the state it thinks it is in, and this is the most plausible reason no
save file has ever appeared on disk.

WHAT TO LOOK AT NEXT: which call site makes the request. The same technique that
worked for the pure-virtual investigation applies -- NtAllocateVirtualMemory can
report its link register, which names the caller, and from there the size's
origin is readable. Do that before guessing a third time.

DO NOT "fix" this by growing the guest heap until the allocation succeeds. A
title that genuinely wants 2.6 GB on a 512 MB console is a title being asked a
question we got wrong somewhere upstream, and making the number fit would hide
it.

### Note (2026-07-29)
THE ALLOCATION CHAIN IS TRACED TO A MULTIPLY. THE COUNT IS THE SUSPECT.

NtAllocateVirtualMemory now reports its link register, which named the caller
and let the rest be read statically:

  sub_82214F50   size = r11 * r25   (mullw, guarded by `twllei r11,0`, so r11
                                     is a count and the title asserts it is > 0)
       |  passes the product as an argument
  sub_82214B58   takes the size in r4, allocates with
                 base=0, type=0x20001000, protect=4
       |
  sub_82612770   the thin NtAllocateVirtualMemory wrapper
       |
  the failure:   wanted 0x9f000000 (2,667,577,344 bytes)

The type value is what pinned the call site: the wrapper has five call sites and
only this one builds 0x20001000 (lis 8192, ori 4096). The other two build
0x20801000, so they were not it.

SO THE SIZE IS count x element-size, and 2.6 GB is what you get when one of them
is wrong. If the element is 768 bytes the count is 3,473,408; the count is read
through `lwzx r5, r31, r8`, i.e. out of a table indexed at runtime.

TWO EXPLANATIONS ALREADY ELIMINATED, both mine:
  - the device capacity I report (capping it to 512 MB changed nothing)
  - the content enumerator being unimplemented (the title never calls it)

WHAT IS PROBABLY NEXT, and it should be measured not assumed: the count comes
from a table, and this title configures pool sizes from its .ini files. A
setting we do not serve, or serve wrongly, would land here as a garbage count.
Worth checking what the title reads before this point -- and separately,
category 0x3 setting 0x2 (XCONFIG_USER_TIME_ZONE_STD_NAME, per Xenia's
xconfig.h) is queried repeatedly just before and answered "unknown setting",
which is a real missing piece regardless of whether it is this one.

THE INSTRUMENT IS THE REUSABLE PART: reporting the link register on a failed
allocation cost two lines and turned "something wants 2.6 GB" into a named
three-function chain in one run. Same technique that cracked the pure-virtual
call (catalog #44).

### Note (2026-07-29)
THE REQUEST IS NAMED, WITH ITS ARGUMENTS, AND ONE OF MY EARLIER CLAIMS WAS WRONG.

A probe on the pool allocator (a strong sub_82214F50 that logs and calls
through) caught both requests in one run:

    140 MB  from 0x82215540: r3=0x401116b0 r5=0x8 r6=0x2 r7=0x0          (succeeds)
   2544 MB  from 0x822153c4: r3=0x401116b0 r5=0x8 r6=0x8 r7=0xb0800000   (fails)

Same allocator object (r3), same alignment (r5=8). The failing one differs in
r6 (8 vs 2) and carries r7=0xb0800000 -- a guest PHYSICAL address, not a size.
So the request is not "give me 2.6 GB of anything"; it is parameterised by a
region.

The call at 0x822153c4 is itself an indirect dispatch -- `vtable = load(this);
target = load(vtable+4); bctrl` -- out of sub_822151F8. Slot 1 of an allocator
interface, which is the same shape of call as the pure-virtual failure in
catalog #44, though there is no reason yet to think they are related.

CORRECTION TO THE PREVIOUS NOTE: I wrote that the allocation type 0x20001000
"pinned the call site", since only one of the wrapper's five callers builds it.
That reasoning was wrong -- the type is built INSIDE sub_82214B58, so every
caller of that function produces it, and it discriminates nothing. The chain in
that note is still right; the justification for one step of it was not.

Reading the two callers settles which is which anyway, and better: the site at
ppc_recomp.14.cpp:23642 computes `r11 * (65536 / r11)`, which cannot exceed
65536 bytes. So the 2.6 GB must come from the OTHER site, which rounds its
argument up to a 64 KB boundary and passes it straight through. That matches
0x9f000000 being 64 KB-aligned.

WHERE IT STANDS: the size is decided above sub_822151F8 and arrives as an
argument. The next step is the same probe one level up, which is cheap now that
the pattern is established. Worth noting 2544 MB is about five times the
console's entire 512 MB, so whatever computes it is not working from a number
this machine gave it correctly.

### Note (2026-07-29)
A REAL BUG FIXED, A PREDICTION FAILED. BOTH RECORDED.

MmGetPhysicalAddress returned its argument unchanged. The console maps one bank
of physical memory into several virtual windows (0xA0000000, 0xC0000000,
0xE0000000, differing only in caching), and a physical address is the offset
WITHIN the bank -- so the window base has to come off. Xenia does exactly this
subtraction in PhysicalHeap::GetPhysicalAddress, including the 0x1000 the
0xE0000000 window carries.

Fixed and verified on real calls: MmGetPhysicalAddress(0xa0000000) now returns
0x0, (0xa0000040) returns 0x40, (0xaa022880) returns 0xa022880. Previously every
one of those returned its input. The title makes about ten of these calls per
boot, so it was getting wrong answers throughout.

THE PREDICTION IT WAS MADE ON WAS WRONG. I expected this to fix the 2.6 GB
request, because 0x4F800000 - 0xB0800000 is exactly 0x9F000000 under 32-bit
underflow, which looked like a virtual address being subtracted from a physical
one. After the fix the request is byte-for-byte identical and r7 is still
0xb0800000. So the arithmetic coincidence remains a coincidence, and whatever
computes that size does not get it through this function.

The fix stays because it is right, not because it helped here.

TIMEBOX. This entry has now consumed several sessions' worth of iterations and
produced four eliminated explanations (device capacity, the content enumerator,
the allocation type discriminating a call site, and physical-address
translation) against one solid chain. It is worth noting that the ASSUMPTION
underneath it all -- that this allocation is why no save file appears -- has
never been tested. The cheaper question is what the title does between mounting
the content and giving up, and whether it ever opens a file at all. Test that
before descending another level.

### Note (2026-07-29)
THE SIZE IS FULLY TRACED, THE ASSUMPTION IS TESTED, AND TWO OF MY OWN CLAIMS
WERE WRONG.

THE ASSUMPTION FIRST, because it was the thing the last note said to test.
"Is this allocation why no save file appears?" YES, and harder than believed:
the title SEGFAULTS. Run to completion without a watchdog and the process exits
139 (SIGSEGV) within a second of the failed allocation. It asks for 2.6 GB, is
told no, and dereferences the null.

  CORRECTION: an earlier note in this entry says "The title does not treat the
  refusal as fatal -- the run continues at 29.4 fps". That is FALSE. It came
  from logs that were cut short by a watchdog a few seconds after the mount, so
  the frames still in flight looked like the title carrying on. Watching 90 s
  past the mount shows the guest stop dead: no VdSwap, no audio counters,
  nothing.

THE FULL CHAIN, every level measured rather than inferred:

  sub_8232B548   r11 = abs(load32(object + 80)); object2->count = r11
       |         r4 = 2 (element size), r5 = 8
  sub_82170408   size = load32(this + 8) * r4      <-- THE MULTIPLY
       |
  sub_82215898   takes a critical section, forwards the size unchanged
  sub_822151F8   the engine REALLOC; null pointer falls through to allocate
  sub_82214F50   the pool allocator
  sub_82214B58 -> sub_82612770 -> NtAllocateVirtualMemory -> refused

Measured at the multiply: 1,333,788,672 elements x 2 bytes = 0x9f000000.

THE COUNT IS A FLOAT BEING READ AS AN INTEGER. The field holds 0x4F800000,
which is not a plausible count in any base -- but as IEEE-754 it is EXACTLY
4294967296.0, that is 2^32. An integer field holding the bit pattern of a float
is the signature of a value that was never converted, and 2^32 specifically is
what a saturating or wrapping conversion of an unsigned produces.

  CORRECTION: a previous note reads r7=0xb0800000 at the allocator as "a guest
  PHYSICAL address, not a size -- so the request is parameterised by a region".
  Wrong. 0xB0800000 is simply -0x4F800000 in two's complement: it is the raw
  field value BEFORE sub_8232B548 takes its absolute value, left in a register.
  There is no region and nothing is parameterised.

  This also finally explains the arithmetic coincidence that sent the last
  session after MmGetPhysicalAddress: 0x4F800000 and 0xB0800000 are not two
  addresses that happen to differ by the size. They are the same number, one
  negated, and the size is just twice it.

WHAT IS STILL OPEN: what writes 2^32-as-a-float into an integer field at
object+80. Two shapes fit and they have very different owners -- the title
storing a float where an int belongs (its bug, and it would have to be a
value we fed it), or a float-to-int conversion that the RECOMPILER emitted as
a raw bit copy (our bug, and a much more serious one, since it would be wrong
everywhere and not just here). Check the recompiled conversion first: it is
cheap and it is the one that would be ours.

THE INSTRUMENT THAT DID THIS: runtime/guest_backtrace.cpp, new. Walking the
guest stack turned a chain that had been costing ONE FULL RUN PER LEVEL into a
single line naming twelve frames at once. Validated three ways before being
believed -- frame 0 matched a link register known independently, frame 1 is a
real return address after a vtable dispatch whose shape matches the callee, and
the outermost frame (0x82612d88) sits just past the image entry point
0x82612bf0, which is where a thread genuinely begins.

ALSO FOUND, unrelated but worth having: the title queries XConfig category 3
setting 2 with a FOUR-BYTE buffer. The previous note guesses this is
XCONFIG_USER_TIME_ZONE_STD_NAME, which is a string -- so either that
identification or the numbering behind it is wrong. Do not implement it from
the guess.

### Note (2026-07-29)
IT IS NOT THE SAVE PATH. THE TITLE OF THIS ENTRY IS WRONG.

The archive doing the serialisation was dumped: at +4 it holds 0x176 (=374,
a UE3 package FILE VERSION) and its +20 "is saving" flag is 0. This is UE3
PACKAGE deserialisation reading an FString, on a LOADING archive -- not a save
being written. The only thing tying it to saves was that the content mount is
the previous line in the log, and adjacency is not ownership. That is the SAME
mistake this project already made once, identifying a movie by what sat next to
it in the log.

What is genuinely established, and it is a lot:
  - the length is READ OUT OF the archive, so the bytes we feed it are wrong.
    This is ours, not the title's.
  - the title SEGFAULTS on the failed allocation (exit 139), so this is fatal,
    not cosmetic.
  - no NtReadFile happens between the mount and the failure, so the archive is
    reading memory that was filled earlier, not streaming from a file.

TWO MORE EXPLANATIONS ELIMINATED THIS SESSION, both tested rather than argued:

  - "We tell the title a save exists when the directory is empty, so it
    deserialises a buffer nothing filled." XamContentCreateEx did have exactly
    that bug -- `existed` tested for the DIRECTORY, which the runtime itself
    creates on the first write, so every later OPEN_EXISTING succeeded against
    an empty save. FIXED (content now exists only if it holds a non-empty file,
    and the mount correctly reports "new" twice). The 2.6 GB request is
    UNCHANGED. Kept because it is right, not because it helped.

  - "WarGame_p.xxx is truncated -- 32768 bytes exactly, and the read was SHORT
    (32768 of 131072 requested)." The header parses cleanly: tag 0x9E2A83C1,
    version 374, FolderName "None", NameCount 183 at offset 97, imports at 5656
    and exports at 6552 -- all well inside 32768. Every .xxx in the extraction
    is a multiple of 32768, so the extractor pads rather than truncates and the
    short read is just the file ending. Not it.

WHERE TO GO NEXT, and NOT one level deeper on the same thread: the archive
object at +0 carries vtable 0x8209e110 and has no buffer pointer in its first
64 bytes, so it is a wrapper and the real reader is the object behind it.
Identify the class from that vtable and find its buffer; the question "which
bytes, from which file, at which offset" is still unanswered and is the only
one that matters. Everything above it in the chain is now known.

RENAME THIS ENTRY when the owner is confirmed. Leaving "on the save path" in the
title would send the next session down the same wrong road this one started on.

### Note (2026-07-29)
THE CHAIN IS CLOSED END TO END. THE SIZE IS EXPLAINED DOWN TO THE BYTE.

The archive's Serialize is vtable slot 1, sub_821B5C28, and it is an unbounded
memory reader:

    Serialize(this, dest, length):
        if (length == 0) return
        buffer = *(uint32*)(this + 112) -> +0     // the byte array
        memcpy(dest, buffer + this->offset(+108), length)
        this->offset += length

There is NO bounds check and NO validity check on the buffer.

THE ARRAY IS EMPTY: its data pointer is 0 and its count is 0. So the read was
literally memcpy(dest, 0 + 4, 4) -- it read LOW GUEST MEMORY. Address 4 holds
0xb0800000, the FString code takes its absolute value (0x4F800000) and
multiplies by 2 bytes per character, and that is 0x9f000000. Every step is now
measured, not inferred, and the number has no mystery left in it.

WHY A NULL READ SUCCEEDS HERE, which is the part worth keeping: guest address 0
is MAPPED, because kPhysicalAliases in guest_memory.cpp includes 0x00000000 --
the raw physical window. The console exposes its 512 MiB through several
windows and the runtime aliases all of them, so the whole physical block is
readable and writable at guest address 0. A null dereference in guest code
therefore does not fault; it reads live physical RAM. That is why the length
was interesting garbage rather than a zero (a zero length would have been
harmless and there would be no bug here at all).

  This is a standing DEBUGGING HAZARD, not a fix to apply blindly. Unmapping
  the first page would make null dereferences fault where the console faults --
  but physical page 0 is inside the physical heap's own range
  (MmGetPhysicalAddress(0xa0000000) is 0), so a physical allocation can legally
  land there and the GPU path would then be dereferencing a guard page. Do not
  unmap it without first proving nothing allocates physical page 0.

WHAT IS LEFT, and it is now a single question: WHY IS THE ARRAY EMPTY? The
deserialisation is a DEFERRED MESSAGE HANDLER -- sub_821B43F8 dispatches on a
message id (48 here, 45 nearby) and only calls the deserialiser when a flag in
the global struct at 0x82BFAD3C is non-zero. So something set "there is data
pending" while the buffer behind it was never filled. Find what sets that flag:
the struct at 0x82BFAD10 is filled at 0x821FB4D8 (three FStrings and a flag at
+40), and 0x82BFAD3C is +44 of the same struct, which nothing else in the image
references by address.

RULED OUT this session, so nobody repeats it: the four XMsg services refused
just before the failure are NOT the missing filler. Identified against Xenia --
0x0007001A/B are XMP playback controller get/set, 0x00058004 and 0x00058020 are
XLiveBase startup and its enumerate, 0x000B0006 is XGIUserSetContext. All are
either fire-and-forget or write a small status into their own buffer; none of
them populates a deserialisation blob. They are still worth implementing as
real services, but not as a fix for this.

### Resolution (2026-07-29)
FIXED by porting the storage-device selector (XamShowDeviceSelectorUI), not by anything in the allocator chain.

THE MECHANISM, end to end:
  the title asks the player which storage device to save to
    -> the runtime REFUSED the dialog (it sat in the no-system-UI list)
    -> the title never obtained a device, so it never established a save destination
    -> its deferred handler still ran, deserialising an FString from an archive whose byte array was EMPTY (data pointer 0, count 0)
    -> Serialize has no bounds check, so that read went to LOW GUEST MEMORY, which is mapped because kPhysicalAliases includes the raw physical window at 0x00000000
    -> address 4 held 0xb0800000; abs() and 2 bytes per character give 0x9f000000
    -> the 2.6 GB allocation is refused and the title dereferences the null

VERIFIED BY A SINGLE-VARIABLE A/B, both directions:
  selector refused  : exit 139 (SIGSEGV, core dumped), the 2.6 GB request present, ~1800 frames
  selector answered : exit 124 (survived the full 400 s timeout), NO oversized request, 11640 frames at 29.8 fps

WHY IT WAS MISSED: every step of the allocator chain was measured correctly and none of them was the cause. The cause was a dialog refused sixty seconds earlier and four subsystems away. Refusing the selector looked locally safe -- it sits with the other system dialogs, which genuinely have no answer here -- but it is the ONE dialog a PC port can always answer, because a PC has exactly one place saves go.

THE LESSON, and the reason this entry ran for several sessions: the bug was not in the chain that produced the number. It was a service that was never implemented, and it only became reachable once the surrounding chain was ported far enough for the title to get there. Chasing the symptom down four levels could not have found it; porting the chain did.

### Reopened (2026-07-29)
CORRECTION TO THE RESOLUTION ABOVE. I OVERSTATED IT.

The claim was 'verified by a single-variable A/B, both directions'. That A/B was CONFOUNDED and I should not have called it single-variable.

Answering the device selector CHANGES THE MENU FLOW: the 'no storage device' dialog stops appearing, so the timed input script that walked the title into the campaign no longer matches the menus. In the selector-ON arm the title never reached the campaign at all -- it idled in a menu. 'Survived 400 s and rendered 11640 frames' was therefore measuring a title sitting still, not a bug that was fixed. Re-running with a script corrected for the new flow made the title reach the checkpoint again, and it SEGFAULTS THERE.

A timed script cannot produce a clean A/B for this change at all, because the change alters the navigation the script depends on. That is a property of the instrument, not of the fix.

WHAT SURVIVES THE CORRECTION, stated at the strength the evidence actually supports:
  - on a run that DOES reach the checkpoint mount, the 2.6 GB request is now ABSENT (0 occurrences) where it was previously present every time. Same milestone reached, allocation gone. That is real evidence the device selector removed this specific failure.
  - it is NOT proof, because the two runs differ in more than one way.

WHAT IS DEFINITELY NOT FIXED: the title still segfaults at the checkpoint, now WITHOUT the oversized allocation. So this entry's symptom is gone and a different failure at the same point is not. Leaving this open until the remaining crash at the checkpoint is identified, because closing it would tell the next session the save path works when it does not.

The device selector implementation stays regardless: refusing a dialog a PC can always answer was wrong on its own merits.

### Note (2026-07-29)
SECOND CORRECTION, AND THE IMPORTANT ONE: THE DEVICE SELECTOR DID NOT FIX THIS.

Measured directly rather than inferred. At the deserialisation the archive's byte array is STILL EMPTY -- 0 bytes, data pointer 0 -- on every single call, exactly as before. Nothing filled it. What changed is what LOW GUEST MEMORY happens to contain:

  before the selector: guest address 4 held 0xb0800000  -> abs, x2 -> 0x9f000000 -> 2.6 GB -> refused -> segfault
  after  the selector: guest address 4 holds 0x00000000 -> a zero length -> an empty string -> no allocation at all

So the oversized request vanished because the garbage became benign, not because the data arrived. Answering the selector changes the title's allocation pattern, which changes what lands in physical page 0, which is what the null read returns. That is luck, and it is exactly the kind of change that makes a symptom disappear without touching its cause.

THE ROOT CAUSE IS UNCHANGED AND STILL THE SAME ONE:
  an FArchive over an EMPTY byte array, read by a Serialize with no bounds
  check, through a null pointer, into a null page that is MAPPED (because
  kPhysicalAliases includes the raw physical window at 0x00000000).

On the console that read would fault, which means the array is NEVER empty there -- so the real defect is that this runtime drives the title into deserialising data it was never given.

The title still segfaults at the checkpoint, now in sub_824961D0: an indirect call through vtable slot 52 of an object at r24+1376 whose 'vtable' pointer is a HEAP address (0x42babe80), not an image one. Same object graph, half-built from the same empty deserialisation.

WHAT I GOT WRONG AND WHY IT MATTERS: I claimed a single-variable A/B proved the fix. It did not -- answering the selector changes the menu flow, so the timed input script no longer navigates the same path, and the two arms were never running the same code. Then even the weaker claim ('same milestone reached, allocation gone') turned out to be measuring memory luck. Two levels of over-claiming on the same change.

THE NEXT STEP IS UNCHANGED FROM BEFORE THE DETOUR: find what sets the pending-data flag at 0x82BFAD3C+0 (it is +44 of the struct at 0x82BFAD10, filled at 0x821FB4D8) while the buffer behind it is empty. That is the service that is missing.

### Note (2026-07-29)
THE PENDING FLAG IS SET UNCONDITIONALLY, AND find_addr_refs.py WAS LYING.

THE INSTRUMENT FIRST, because it invalidates an earlier note in this entry.
tools/find_addr_refs.py recognised only 'lis rA,hi ; addi rA,rA,lo' -- the
address COMPUTED into a register. It did not recognise 'lis rA,hi ; stw rD,lo(rA)',
where the address is used directly by a load or store. Every global whose only
writer uses the second shape was reported as having NO writers.

That is exactly what happened here. An earlier note in this entry says 'only ONE
reference to 0x82BFAD3C in the whole image (the dispatcher itself)' and reasons
from that absence. FALSE. With the tool fixed there are THREE:
    0x821b4440  the dispatcher's read
    0x821b44f4  the dispatcher's msg-45 clear
    0x82207dbc  THE SETTER -- which was invisible
Fixed in tools/find_addr_refs.py, validated both ways: the five addi-form
references to 0x82BFAD10 are unchanged, and a control address still reports
nothing. An instrument that under-reports SILENTLY is worse than none, because
'no hits' reads as an answer rather than as a blind spot.

WHAT SETS THE FLAG: sub_82207CC8 at 0x82207DBC does 'li r11,1 ; stw r11,-21188(r10)'
with r10 = 0x82C00000. It is a Kismet SequenceOp: on its FIRST TICK (member +228
== 0) it plays the movie Startup.sfd, registers the observer for messages 48 and
45, and then sets the flag UNCONDITIONALLY and +228 = 98. IT PERFORMS NO CHECK
THAT ANY DATA EXISTS.

So the title is not wrong to announce pending data -- announcing it is
unconditional by design. The defect is that the buffer behind it was never
filled. The blob is the TArray<BYTE> at offset 420 of *(APlayerController+1496),
and its feeder is sub_821BA838, which RETURNS IMMEDIATELY at 0x821BA850 when
[obj+424] == 0.

NEXT STEP, and it is cheap: one instrumented run logging (1) [obj+424] and
whether sub_821BA838 gets past its early return, (2) whether sub_82207E98 runs
and its 'LoadChapter=%i' parse succeeds, (3) the return of the virtual at
[*(PC+1496)]+656 called at 0x821B4928. Those three discriminate the candidates.

DO NOT add a 'skip the deserialise if the array is empty' guard. That is a
bandaid over an unexplained state, and the state is now nearly explained.

### Note (2026-07-29)
THE CHAIN IS COMPLETE. THE BLOB IS EMPTY BECAUSE ITS SOURCE IS EMPTY.

Measured this run, not inferred:

  [probe] FString deserialise #1: archive array is EMPTY (0 bytes at 0x0)
  [probe]   the global carrier at 0x82bfb36c is EMPTY (0 bytes at 0x0)
          -- this is what gets copied into the object before the read

THE MECHANISM, end to end, every step read from the recompiled code:

  1. sub_82207CC8 (a Kismet SequenceOp) sets the pending flag at 0x82BFAD3C on
     its FIRST TICK, unconditionally, with no check that any data exists.
  2. The deferred dispatcher sub_821B43F8 sees the flag on message 48 and calls
     sub_821B4620.
  3. sub_821B4620 calls a virtual; if it returns ZERO (guest 0x821B4928:
     ) it copies the GLOBAL TArray at 0x82BFB36C into
     object+420 via sub_821B5C90.
  4. That global is EMPTY, so the copy faithfully produces an empty array.
  5. The archive then reads through a null data pointer into low guest memory,
     which is mapped (the raw physical alias), and gets whatever is there.

So nothing is corrupt anywhere. Each step does exactly what it should with the
input it is given, and the input is empty.

RULED OUT BY MEASUREMENT: sub_821BA838, which builds a SAVING archive over the
same object+420 and was the obvious candidate for the filler, IS NEVER CALLED.
A probe on it produced no output across a full run while a sibling probe on the
same path fired four times, so the seam works and the absence is real. It has no
code references in the image (find_addr_refs) and appears as a data word in a
vtable at 0x8209F6A8 -- a virtual that is simply never invoked here.

WHAT FILLS THE CARRIER: sub_82207E98, the Kismet op that parses
'LoadChapter=%i', copies object+420 INTO the global (guest 0x82208034:
). So the carrier is
written when a chapter load is requested and read back after the map change.

THE OPEN QUESTION, and it is now a sharp one: on this run the title reaches the
restore with an EMPTY carrier, which means the chapter-load op never ran, or ran
without state to carry. Two candidates worth one run each:
  (a) the title expects a 'LoadChapter=N' option in its launch data / URL and we
      supply none (GEARS_CMDLINE exists and is handed over as launch data);
  (b) the scripted input reaches the level by a route that skips the op.
Probe sub_82207E98 for entry and for its parse result to tell these apart.

DO NOT add a guard that skips the deserialise when the array is empty. The array
being empty is not the defect -- it is the faithful consequence of a carrier
that was never filled, and guarding it would hide which of (a) or (b) is true.

### Note (2026-07-29)
CORRECTION, AND IT INVALIDATES THE PREVIOUS NOTE. THE ARRAY IS NOT EMPTY AT THE CRASH -- MY PROBE WAS CAPPED AND NEVER LOOKED.

The deserialise probe reported only its first FOUR calls. There are 78,278 in a
run. So every 'the archive array is EMPTY' line in this entry describes the
first four calls, which happen early, before anything fills the carrier, and are
harmless. The instrument had stopped looking long before the interesting one.

That is the same blind-detector failure this session has now hit four times, and
this one was in code I wrote, in the probe I was using to reason about this very
bug. 'No populated arrays observed' was a conclusion the instrument could not
have contradicted.

WITH THE CAP REMOVED, the state AT THE CRASH is:

    FString deserialise #78278: archive array is POPULATED
        (2147483648 bytes at 0x900006b0);
        guest address 4 currently holds 0xb0800000

2147483648 is 0x80000000 -- not a plausible byte count, and 0x900006b0 is not a
valid guest heap or physical address. So the array DESCRIPTOR is corrupt, not
absent, and the title is looping over it: 78,278 deserialises in one run is a
runaway, not a single failed read.

ALSO CORRECTED: the earlier claim that the carrier at 0x82BFB36C is empty and
the LoadChapter op never runs. Both are false. Measured ordering in one run:

    line  63-69   four deserialises, carrier EMPTY, harmless
    line 201-203  THE LoadChapter OP RUNS: carrier EMPTY -> POPULATED,
                  385 bytes at 0x434f1f80
    line 206      content mounted 'new' (the save is created)
    line 269-270  the startup Kismet op runs; carrier is POPULATED
    line 273      content mounted 'existing' -- the restore path
    line 274      core dump

385 bytes is exactly the size of D:\WarGame\Checkpoints\chapter37.sav, which
the runtime reads earlier. So the carrier holds real checkpoint bytes.

THE QUESTION IS NOW A DATA-INTERPRETATION ONE, which is much better than
'why is it empty': 385 valid bytes go in, and a descriptor claiming 2^31
elements at an impossible address comes out. Either those bytes are not what the
title expects at that offset, or the archive's position/endianness is off, or
what we hand back for that file differs from the console's. The next step is to
dump the 385 bytes and hand-decode the FString/TArray headers the title expects,
against the deserialise sequence.

### Note (2026-07-29)
THE CRASH OBJECT IS A FREED HEAP BLOCK. USE-AFTER-FREE, NOT BAD DATA.

Confirmed from the actual core file, independently re-derived on review:

  ctx.r24 = 0x41c84e00 ; ctx.r3 = 0x42babc40 ; ctx.ctr = 0x0
  -> a NULL indirect call, not a jump to 0xFF as I reported earlier from a
     live gdb session (that reading was wrong; [0x42babeb4] = 0).

The object at 0x42babc40 is ON A POOL FREE-LIST: length 214, terminator
00000000, every node's word1 == 1, every node 0x40-aligned, strides
[192,384,768,2304,4032,8064], span 0x42ba0240..0x42badd40, and the pool
descriptor at 0x41421750 holds 0x42babc40 as its HEAD. The 'vtable' pointer
0x42babe80 that I flagged as suspicious is itself a freed block in the same list
(word0 = 0x42ba0240, word1 = 1).

So the object was FREED and then used. Its slot-0 word is a free-list next
pointer, which is why the 'vtable' looked like a heap address.

The real class is identifiable: sub_8242C098 installs vtable 0x821047A8 after
calling the FArchive base constructor at 0x821B5B78; that vtable's slot 13
(byte +52) is 0x824841A8, which is the call that faults.

THIS SUPERSEDES THE PREVIOUS TWO READINGS IN THIS ENTRY. It is not an empty
array (my probe was capped and never saw the crash-time state), and it is not a
corrupt descriptor deserialised from bad bytes (that descriptor is a symptom of
reading a freed block). It is an object lifetime bug: something frees this
archive-derived object while the restore path still holds it.

NOTE ON THE CARRIER: the earlier finding stands -- LoadChapter runs and fills
the carrier with 385 real bytes. That is NOT the problem. The problem is what
happens to the object built over it.

### Note (2026-07-29)
THE FREE IS NAMED: IT COMES FROM THE ENGINE REALLOC.

A new probe on the pool's free (sub_822153F0, identified from the allocator's
own vtable at runtime -- slot +4 allocate, +8 realloc, +12 FREE) plus
GEARS_WATCH_FREE=<address> reports the release of a specific block as it
happens. Run with the address the core file gave (0x42babc40):

  [lifetime] watching for the release of 0x42babc40
  [lifetime:error] WATCHED ADDRESS 0x42babc40 FREED from 0x822153e4 (free #742078)
  [lifetime:error] WATCHED ADDRESS 0x42babc40 FREED from 0x82215944 (free #1848647)
  [lifetime:error] WATCHED ADDRESS 0x42babc40 FREED from 0x822153e4 (free #1873577)

The last one is immediately before the crash. Both sites are inside the
allocator itself:
  0x822153E4 is in sub_822151F8, THE ENGINE REALLOC (the return address of its
             free-the-old-block call)
  0x82215944 is in sub_82215648

BEING FREED THREE TIMES IS NOT ITSELF A BUG -- a pool reuses addresses, so
alloc/free/alloc/free on the same address is normal. What matters is that the
last free comes from the REALLOC path, which is the classic shape for this
crash: the array grows, realloc moves it, the old block is released, and
something still holds the OLD pointer. The restore path then calls through it.

WHAT IS STILL NEEDED to close this: show that the pointer the restore path uses
(the object at r24+1376 inside sub_824961D0, confirmed as 0x42babc40 by gdb) is
a copy taken BEFORE that realloc. That is a matter of reading who caches it.

INSTRUMENT NOTE, because it cost a run: the obvious probe -- a strong
sub_824961D0 reading r24+1376 on entry -- IS INVALID. r24 is set up inside that
function, so on entry it holds the caller's value; the probe reported an object
of 0x470075 with a null vtable, which is nonsense. A probe can only see a guest
function's arguments and callee-saved registers after the prologue. For
mid-function state the core file is the instrument. Also removed: a
constructor/destructor probe pair that reported '0 constructed, 0 destroyed' --
those bodies are inlined at their call sites, so that seam is blind and its zero
means nothing. Both are now documented in the source so the next reader does not
repeat them.

### Note (2026-07-29)
THE CACHE AND THE USE ARE IN THE SAME FUNCTION, AND THE USE IS GUARDED.

sub_824961D0 -- the function that faults -- both CREATES the object and CACHES
it. At ppc_recomp.68.cpp:5531:

    bl 0x8242c098        ; construct (this is the class the core file named,
                         ;  vtable 0x821047A8, installed after the FArchive base
                         ;  constructor at 0x821B5B78)
    mr r3,r19
    stw r3,1376(r24)     ; cache the pointer in the holder

There are SIX stores to r24+1376 in this one function, so the field is a cache
that is reassigned along different paths.

The faulting read, at ppc_recomp.68.cpp:6271, is GUARDED:

    lwz  r11,1396(r24)
    cmpwi cr6,r11,0
    bne  cr6,<skip>      ; only use the cache when +1396 is ZERO
    lwz  r3,1376(r24)
    lwz  r11,0(r3)
    lwz  r11,52(r11)     ; slot 13
    bctrl                ; <-- faults

So the title is not blindly dereferencing a cache; it consults +1396 first. At
the crash +1396 must therefore be zero while the object at +1376 is dead.

WHAT +1396 IS, honestly: NOT yet established. The only writer of that offset on
what looks like this object type (sub_82496AE0, ppc_recomp.68.cpp:7040) stores a
POINTER (r24) rather than a flag, which suggests +1396 is a SELECTOR -- 'use the
object here if present, otherwise the cache at +1376' -- rather than a validity
bit. The other two writers of +1396 in the image (sub_822396A8) are reached
through a float conversion on a different structure and are almost certainly a
different field of a different class. I am not claiming +1396's meaning.

COMBINED WITH THE MEASURED FREE: the block at +1376 is released by the guest's
OWN pool realloc (sub_822151F8, free site 0x822153E4), and the pool is guest
code we do not implement -- so it behaves as it does on console GIVEN THE SAME
SIZES. That is the thread worth pulling: if the title reallocs where the console
would not, the difference is in a SIZE we supply, not in the allocator.

NEXT: dump +1376 and +1396 from the core at the fault to confirm the guard state
directly rather than by inference, and find which of the six stores put the dead
pointer there.

### Note (2026-07-29)
THE GUARD STATE IS NOW MEASURED, NOT INFERRED.

From the core at the fault:

    holder      = 0x41c94e00
    cache +1376 = 42babc40      <- the freed pool block
    guard +1396 = 00000000      <- zero, which is the 'use the cache' branch

So the title takes the cached path because +1396 is zero, and the cached pointer
is dead. Nothing invalidated it. That is the defect in one line: a cache whose
invalidator either does not exist on this path or did not run.

The next question is narrow and answerable: does anything ever write a non-zero
+1396 on THIS object, and is it supposed to run before the free? The only
candidate writer found so far (sub_82496AE0) stores a POINTER there rather than
a flag, so +1396 may be a selector rather than a validity bit -- in which case
the invalidation lives elsewhere and the search is for whoever should clear
+1376 when the block is released.

### Note (2026-07-29)
MEASURED END TO END: THE OBJECT IS FREED WHILE STILL CACHED, 156 TIMES A RUN.

A probe on the pool's free (sub_822153F0) that checks every live holder's
+1376 against the address being released:

  FREEING 0x42bad200 ... at holder 0x41727800+1376 (occurrence 155)
  FREEING 0x42babc40 ... at holder 0x41c94e00+1376 (occurrence 156)

Occurrence 156 is exactly the object and exactly the holder that gdb reports at
the fault. So the crash is the 156th instance of a thing that happens all run:
the title caches an object at holder+1376, the pool releases it, and NOTHING
nulls the field.

WHY THE FIRST 155 ARE SURVIVED: a freed pool block usually still contains the
old object's bytes, so the vtable read happens to work. The run dies when the
block has been REUSED and its first word is a free-list next pointer instead --
which is precisely the state the core file showed (head of a 214-node list).

HOW THE HOLDER IS OBTAINED, since an earlier probe got this wrong: sub_824961D0
takes the holder in r3 and does  immediately. r24 at ENTRY is still
the caller's value -- reading it there produced 0x470075 and a null vtable,
which was nonsense. r3 at entry is correct.

AND THE PROLOGUE SHOWS THERE IS NO VALIDITY CHECK:
    lwz r11,1376(r24) ; cmplwi cr6,r11,0 ; bne cr6,<use it>
The cache is used whenever it is merely NON-NULL.

INSTRUMENT NOTE, and it is the reason this was found at all: the first version
of this probe capped at 'first 6 occurrences' and reported events 1-6 -- all
early, all survived, none the crash. Capping the BORING case hid the only one
that mattered. It now counts every occurrence and reports by novelty, and the
occurrence number goes out with each line so elision is visible.

REMAINING QUESTION, now narrow: is that free legitimate? The pool is guest code
and behaves as on console given the same inputs, so if the title releases this
block where the console would not, the difference is in a SIZE we supply.

### Note (2026-07-29)
THE 'A SIZE WE SUPPLY' HYPOTHESIS IS ELIMINATED. THE REALLOC IS A DELETE.

Probing the engine realloc for calls whose existing block is currently cached at
some holder's +1376:

  REALLOC of 0x43f68980 to 0 bytes from 0x822158e4 -- cached at holder
      0x491fc600+1376, so this call orphans it (occurrence 1)
  ... 100 occurrences, EVERY ONE to 0 bytes ...

realloc(ptr, 0) is the free idiom. So these are not the array growing and moving
-- there is no size involved at all, and nothing this runtime supplies drives
them. The title is DELETING the object and leaving the cache pointing at it.

That kills the line of enquiry the previous note proposed ('if the title
reallocs where the console would not, the difference is in a size we supply').
Measured, not argued.

WHAT THE SHAPE ACTUALLY IS: one holder (0x491fc600 in this run) accounts for all
100, each with a DIFFERENT block. So the field is a cache that is reassigned
repeatedly -- object cached, object deleted, field left stale until the next use
overwrites it. That is a WINDOW, not a permanent leak, and the crash is a use
that lands inside the window:

  1. cache X at +1376
  2. delete X (realloc to 0)   -- +1376 still points at X
  3. call sub_824961D0        -- prologue sees +1376 non-null, USES dead X

NEXT: find whether the deleting path is supposed to null +1376 as well. The free
arrives from 0x822158e4, which is the locked allocator wrapper -- i.e. the
generic deallocator, so the interesting caller is one level further out. If the
title's own delete does clear the field, then something about OUR ordering lets
step 3 happen before the clear; if it never clears it, the guard must be
something other than non-null and the +1396 selector needs identifying.

### Note (2026-07-29)
CROSS-THREAD USE-AFTER-FREE. MEASURED ON BOTH SIDES, AND IT LINKS #45 TO #44.

  the USER of the cache, sub_824961D0, runs on guest thread 'host'  (game thread)
  the DELETER runs on 'guest-7'                                     (render thread)

The deleter's stack, from a walk at the orphaning realloc:

  0x822158e4 <- 0x82170470 <- 0x8232ae10 <- 0x825552a4 <- 0x82551240
    <- 0x825552fc <- 0x8250b284 <- 0x825550f4 <- 0x82444f7c
    <- 0x82445038 <- 0x8243ae00 <- 0x827a94ac

0x82444F7C is the render loop's Execute() call site -- the same address catalog
#44 identifies as its faulting call. So the object is destroyed from INSIDE a
render command, on the rendering thread, while the game thread still holds it
cached at holder+1376.

So the sequence is:
  game thread    caches the object at holder+1376
  render thread  executes a command that DELETES it (realloc to 0 bytes)
  game thread    calls sub_824961D0, whose prologue takes the cache because it
                 is merely non-null, and calls a virtual on freed memory

THIS IS THE SHAPE UE3 HAS A MECHANISM FOR, and that is the next thing to check.
The game thread is supposed to hand ownership to the rendering thread and then
WAIT on a fence before touching the object again. FenceCommand exists in this
image (vtable 0x82106D58, Describe returns the literal 'FenceCommand'; its
Execute is lwsync + InterlockedDecrement on [this+4], returning 8). If the game
thread's wait for that fence does not actually block in this port -- a stubbed
kernel wait, a counter that never reaches the value it spins for, a completion
signalled too early -- then the game thread proceeds while the render thread
still owns the object, and this is OURS.

That is now the single highest-value question for this entry: find the game
thread's fence WAIT and prove it actually waits. Note the earlier finding that
lwsync is translated faithfully and the guest loads are volatile, so the memory
model is not the suspect -- the suspect is a kernel primitive or a completion we
signal.

### Note (2026-07-29)
THE FENCE IS NOT BROKEN. HYPOTHESIS ELIMINATED BY MEASUREMENT.

Last note proposed that the game thread's fence wait might not actually block in
this port, letting it proceed while the renderer still owned the object. It
does block. sub_824453A0 (FRenderCommandFence's wait: spin on [fence+0] until it
falls to the threshold in r4, yielding via sub_826128B8 between reads) was
probed:

  wait #1 on 0x401025c0: counter 1 vs threshold 0 -> WILL BLOCK ...
  ... 8 waits in a run, EVERY ONE of them blocking ...

So the mechanism runs, the counter is genuinely above the threshold when it is
consulted, and the loop genuinely waits. Nothing in this port defeats it.

BUT THE NUMBERS ARE THE POINT: 8 fence waits per run against 156 deletes that
orphan a cached pointer. The fence is used for a handful of specific handoffs,
and the cached object at holder+1376 is NOT one of them. So the protection
exists in the engine and simply is not applied on this path.

WHAT THAT LEAVES. The game thread enqueues a render command that DELETES the
object (the deleter's stack ends at the drain loop's Execute), and then uses its
own cached pointer to that object without any fence in between. On the console
that is the same code with the same absence of a fence -- so either

  (a) the game thread is supposed to null +1376 when it enqueues the delete, and
      something about our path skips that store; or
  (b) it is a genuine title race that the console's timing hides, in which case
      it belongs with catalog #44's non-atomic commit as the same class of
      problem: a latent race this port exposes by running the two threads truly
      in parallel where Xenon interleaved them on shared cores.

(a) is checkable and should be checked first: find the enqueue site for the
deleting command and look for a store of 0 to +1376 near it.

### Note (2026-07-30)
THE DELETING COMMAND IS FDrawSceneCommand, AND THE DELETE GOES THROUGH THE CONTAINER HELPER.

Resolving the deleter's stack to functions (each address decoded, not guessed):

  0x822158e4  the locked allocator wrapper
  0x82170470  sub_82170408   the container size helper (size = count x element)
  0x8232ae10  sub_8232AE10
  0x825552a4  sub_82555230
  0x82551240  sub_825511D8
  0x825552fc  sub_825552C8
  0x8250b284  sub_8250B228
  0x825550f4  sub_825550A8   <- FDrawSceneCommand::Execute
  0x82444f7c  the drain loop's Execute call site

So the renderer, executing FDrawSceneCommand, ends up calling the container
helper with size 0 -- i.e. emptying a container -- and that releases the block
the game thread has cached at holder+1376.

That is the same sub_82170408 this entry probed much earlier, when it was
computing  and producing 0x9f000000. It is the engine's single
array-storage sizing routine; a size of 0 through it is a clear, not a growth.

WHAT IT MEANS FOR OPTION (a): the game thread does not merely enqueue 'delete
this object' -- it enqueues a DRAW, and the draw's own execution clears a
container as part of its work. So there is no obvious 'delete' call site next to
which a null-the-cache store would sit. That makes (a) much less likely than it
looked: the cache is invalidated, if at all, by whatever normally re-populates
that container, not by the enqueue.

WHICH LEAVES (b) AS THE LEADING EXPLANATION, and it now has company: this is the
same shape as catalog #44 -- title code that is racy in principle, survives on
hardware, and is exposed by a port that runs the game and render threads with
real parallelism where Xenon interleaved them on shared cores. Both would be
addressed by the same port-level decision rather than by separate patches.

BEFORE ACTING ON (b), the thing to establish is the one #44 also needs: why the
console tolerates it. Guessing at thread topology and then serialising things is
how a port acquires permanent unexplained locks.

### Dead end (2026-07-30)
SUSPEND/RESUME RENDERING IS A DEAD END FOR THIS BUG -- THE TITLE NEVER USES IT.

The lead was good and is worth recording so nobody re-walks it. The title has
four render commands -- SuspendRendering1/2, ResumeRendering1/2, identified from
their Describe thunks returning exactly those UTF-16 literals at 0x821044D4,
0x821044F8, 0x8210451C, 0x82104540. Their Execute bodies do one thing each:

    SuspendRendering:  [0x82BFA388] = 1 ; return 4
    ResumeRendering:   [0x82BFA388] = 0 ; return 4

AND NOTHING READS THAT FLAG. find_addr_refs finds exactly two code references to
0x82BFA388 and both are these setters -- the same shape as the ring's bIsWriting.
(Bound: the scanner covers lis+addi and lis+load/store, so a reader obtaining the
address another way would be missed.)

If the title had suspended rendering around the checkpoint restore, this port
could have closed the #45 race honestly by honouring a flag the title already
maintains. IT DOES NOT. Probes on both Execute bodies report ZERO executions in a
full run to the crash.

THE ZERO IS VALIDATED, because 'never called' and 'probe broken' are otherwise
the same output: both addresses are present in the indirect-call mapping table
(so a vtable dispatch reaches the strong override), and two other overrides in
the same file demonstrably fired in the same run. So the seam works and the
absence is real.

CONCLUSION: the title does not suspend rendering around the restore, so there is
no title-maintained flag to honour here. #45 cannot be closed this way.

### Note (2026-07-30)
THE DECISION, WRITTEN UP RATHER THAN QUIETLY TAKEN.

Every mechanism with a port-side fix has been eliminated (five of them, each by
measurement). Both remaining crashes are TITLE races. The engine maintains two
flags that would have given a legitimate seam -- the ring's bIsWriting and a
SuspendRendering flag -- and nothing reads either; the suspend commands never
execute at all. So there is no title protocol left to complete, and that is what
makes this a decision rather than a bug hunt.

OPTION A -- PORT-LEVEL MUTUAL EXCLUSION.
Hold a lock across render-command Execute and across the game thread's restore
path, so the two threads cannot interleave where the title assumes they will not.
  cost: serialises the two threads at a seam the title does not define, so it is
        a lock we invent -- the thing this project's rules forbid. Loses real
        parallelism, changes global timing (the audio pump is already rate
        sensitive), and risks deadlock against the guest's own spins.
  buys: probably both crashes, since #44 is plausibly downstream of #45.
  verdict: NOT recommended as a first move. An invented lock that works is
        indistinguishable from one that hides something, and we would never learn
        which.

OPTION B -- ACCEPT AND MOVE ON. (RECOMMENDED.)
Record both as known title races, keep the instruments in the tree, and spend the
next effort on subsystems that are missing rather than racy.
  cost: the checkpoint RESTORE path stays broken, so load-a-save does not work.
        Save WRITING does.
  buys: the port keeps moving. Everything up to the restore now works, and the
        crash is confined to one path reached only when a save already exists.
  why it is not defeat: the two issues are characterised to the instruction, both
        have live instruments, and #44 has a cheap test that only becomes
        available once #45 moves. Nothing is lost by waiting; the evidence is
        already written down.

WHAT WOULD CHANGE THE ANSWER, so this is falsifiable rather than a shrug:
  1. Console-side evidence that the pool defers reuse of freed blocks. That would
     make the crash a consequence of OUR pool timing after all, and give a
     legitimate fix.
  2. Finding any reader of bIsWriting or 0x82BFA388 that the scanner missed --
     that restores a title protocol to honour. The scanner's bound is stated:
     lis+addi and lis+load/store in code, so a reader obtaining the address
     another way is not excluded.
  3. Evidence that the title never reaches the restore on a real console with a
     fresh profile -- which would mean the port drives it somewhere it should not
     go, and the fix is upstream in the flow rather than in the race.

### Note (2026-07-30)
THE SAVE THE TITLE WRITES IS CORRECT. VERIFIED BYTE-FOR-BYTE.

Closing a gap this project had left open: the save file was known to be CREATED
but its contents had never been checked, which is the difference between 'a file
appeared' and 'the save path works'.

  written: ~/.local/share/gears1/default_checkpoint_sav/Pla   385 bytes
  source:  scratch/game/WarGame/Checkpoints/chapter37.sav      385 bytes
  cmp: BYTE-IDENTICAL

And it decodes as a well-formed UE3 record rather than merely matching:

  +0x00  00000002       version
  +0x04  00000025       37 -- which is the 37 in chapter37.sav
  +0x10  0000000c       FString length 12
  +0x14  "sp_prison_p\0"  the Act 1 prison map, exactly 12 bytes

A positive length is the UE3 convention for an ANSI string, and 12 is the exact
byte count including the terminator -- so the string encoding is right, not just
the byte count.

WHAT THIS ESTABLISHES: the whole write pipeline is correct end to end -- the disc
checkpoint is read, carried through the Kismet LoadChapter op into the global
carrier, and written to the player's save unchanged. Every layer this port added
(profile, content dispositions, the overlapped protocol, directory handles,
gamertag truncation) is doing its job.

WHAT IT DOES NOT ESTABLISH: that a save can be LOADED. No save has ever been read
back into the title, because the restore path is where this entry's crash lives.
Writing is verified; reading is untested and blocked.

### Note (2026-07-30)
TWO NATIVE FIXES ATTEMPTED, BOTH FAILED, AND THE SECOND FAILURE POINTS SOMEWHERE NEW.

The framing is right and stays: this is a PORT, so 'the title's code races' is not
an end state -- we own the behaviour. What was wrong was WHICH invariant to own.

ATTEMPT 1 -- clear the reference. On the pool free, null any holder+1376 that
points at the block being released. The stated reasoning was that freeing must
clear references to it, and that the title would then take a create-a-new-one
path. IT DOES NOT. Measured: the crash moved one instruction earlier with r3 = 0.
The prologue's non-null test is a DIFFERENT decision point; the site that faults
reads +1376 and dereferences it unconditionally, guarded only by the +1396
selector. I asserted the create-path without checking for it -- a use-after-free
became a null dereference, which is not progress.

ATTEMPT 2 -- keep the block alive. Skip the free when the block is still cached,
accepting a bounded leak (156 per run) against a guaranteed crash. The hook
engages and blocks are kept, and the crash is UNCHANGED at the same frame count.

WHY ATTEMPT 2 PROBABLY FAILED, from the fault state: the HOLDER is 0x42b40940 --
itself in the pool's 0x42b range, alongside the freed objects, and it differs on
every run. So the holder is a POOL ALLOCATION too, and the dangling pointer may be
the holder rather than the object it caches. If holder+1376 is being read out of
memory that has been recycled, then protecting the cached object cannot possibly
help, and every measurement built on 'the object at +1376 is stale' has been
looking one level too deep.

STATE OF THE TREE: both interventions are REVERTED. The detection stays, and the
source carries the reasons so neither is rebuilt. Leaving a leak in place for no
measured benefit would be worse than the crash.

NEXT, and it is a different question from the last several: establish whether the
HOLDER is freed while still in use, using the same technique that worked for the
object -- watch the holder address across the pool free. If it is, this entry has
been chasing the wrong pointer since the core-file analysis, and that analysis
needs re-reading with the holder as the suspect.

### Note (2026-07-30)
MEASURED: THE HOLDER ITSELF IS FREED BY THE POOL. SUGGESTIVE, NOT YET CONCLUSIVE.

Following the relocation from the last note -- that the holder sits in the pool's
own address range -- a check on the pool free against the set of holders the title
has passed to sub_824961D0:

  THE HOLDER ITSELF IS BEING FREED: 0x41727800 ... (occurrence 1 of 18 tracked)
  THE HOLDER ITSELF IS BEING FREED: 0x41727200 ... (occurrence 2 of 18 tracked)
  THE HOLDER ITSELF IS BEING FREED: 0x48f16000 ... (occurrence 3 of 18 tracked)
  ... 6 occurrences, 18 holders tracked ...

So holders are pool allocations and the pool does release them. That is the level
the last two failed fixes were missing: anything read at holder+1376 after its
holder is freed comes out of recycled memory, which is why protecting the CACHED
object could not help.

WHAT THIS DOES NOT ESTABLISH, stated because it would be easy to bank it as a
result: a holder being freed is entirely normal once the title has finished with
it. The tracking set never removes entries, so a later free of a RECYCLED address
at the same location also matches. What is needed is narrower -- was the CRASHING
holder freed BEFORE the crashing call, and not legitimately reallocated to a new
object in between.

THE NEXT CHECK IS PRECISE, and is the last one this line of enquiry needs: record
a free ordinal per holder address and an allocation ordinal alongside it, then at
the fault report whether the crashing holder's most recent event was a FREE rather
than an allocation. Free-then-used is the bug; free-then-reallocated-then-used is
the title behaving correctly and means the crash is elsewhere again.

INSTRUMENT NOTE: the first run of this check reported 0 events from a STALE
BINARY -- the build had failed and the run went ahead anyway because the commands
were chained with ';' rather than '&&'. The zero looked exactly like a real
negative. Rebuilt and re-run before anything was concluded.

### Note (2026-07-30)
TWO MORE ELIMINATIONS, AND AN HONEST STATEMENT OF WHERE THIS IS.

Both with real denominators this time.

1. THE HOLDER IS NOT USED AFTER BEING FREED. Pool allocations and frees are now
   ordinal-stamped per address, and sub_824961D0's entry asks which came last for
   the holder in r3. Measured: 18 holders tracked, 6 of them freed by the pool,
   and ZERO entered the function with a free as their most recent event. Holders
   are freed but never walked afterwards, so the relocation proposed two notes ago
   is wrong.

2. THE CACHED VALUE IS A LIVE BLOCK AT ENTRY. The same check applied to
   holder+1376 reports 'a live block (last event an allocation)' every time. So
   the pointer is valid when the call begins -- it is not garbage that was never a
   pointer, and it is not already stale on the way in.

WHICH RETURNS TO THE CROSS-THREAD READING and sharpens it: the block is live at
entry and destroyed DURING the call by the rendering thread. That is consistent
with everything measured, and it means the earlier 'keep the block alive' attempt
should have caught it. It did not, so the fault is in that hook's timing rather
than in the diagnosis -- the object that crashes is cached by a store INSIDE the
call, and the hook may be consulting the field before that store lands.

HONEST STATE: five mechanisms eliminated on this entry, two native fixes
attempted and reverted, and the crash is unchanged. The diagnosis has been stable
for several rounds -- game thread holds a pointer, render thread destroys the
object mid-call -- and what is failing is the INTERVENTION, not the
understanding.

The next intervention worth trying is the one that follows from that: instead of
consulting the cache at free time, RECORD the object at the moment the call
stores it (the six stores to +1376 inside sub_824961D0) and protect it for the
duration of that call. That is a narrower and better-founded hook than either
previous attempt. If it also fails, this needs a fresh pair of eyes rather than a
fourth variation from me.

### Note (2026-07-30)
THE BARRIER MEASURED ZERO, AND THE ZERO IS THE FINDING.

Built a reference barrier (runtime/reference_barrier.{h,cpp}, 8 threaded tests
written first) that makes the pool's free WAIT until no reader is inside
sub_824961D0 holding that block. Delay rather than the earlier attempt's skip,
because suppressing a free leaks the block and desynchronises the pool.

Wired to both seams and run: the barrier engaged ZERO times, while the existing
probe reported 119 frees of a still-cached block in the same run. Those two
numbers together are only consistent with one reading, and it is NOT the one this
entry has carried for several rounds:

  THE OBJECT IS NOT FREED DURING THE CALL. It is freed BETWEEN calls. The stale
  pointer sits in holder+1376 while nobody is executing the function, and the
  crash happens on a LATER call that reads the field and uses it because it is
  non-null.

That explains two things that did not fit before: why the cached value is always
a live block at ENTRY (the entry that crashes is a later one), and why both
previous interventions missed -- they guarded the call, and the damage happens
outside it.

WHAT THIS MAKES THE REAL QUESTION. The field is a cache with no invalidation:
the prologue is  and a non-null test, and nothing in the image
nulls it. So the console relied on whoever destroys the object to clear the
holder's reference to it. That is a concrete mechanism to find, not a race to
paper over.

NEXT, AND IT IS A MEASUREMENT NOT A GUESS: at free time, scan the block being
freed for a word equal to the holder's address. If the object carries a back
pointer to its holder, the destructor is meant to clear holder+1376 through it,
and that code exists in the image and is not running. If it carries no back
pointer, the invalidation is someone else's job and the search moves to whoever
owns both.

The barrier stays: it is correct, tested, and its measured non-engagement is what
ruled the during-the-call reading out. It should be REMOVED from the free path if
the next step confirms the between-calls mechanism, since machinery that never
fires is indistinguishable from machinery that is broken.

### Note (2026-07-30)
THE OBJECT CANNOT INVALIDATE ITSELF, AND NOTHING ELSE NULLS THE FIELD EITHER.

Two measurements, both clean.

1. NO BACK POINTER. At every free of a still-cached block the first 512 bytes of
   the dying object were scanned for a word equal to its holder's address. 24
   occurrences, ZERO hits. The object does not know its holder, so it cannot
   clear the reference on the way out. The 'the destructor should be nulling it'
   reading is dead.

2. NO INVALIDATING STORE EXISTS IN THE IMAGE. All 11 stores to +1376 across the
   whole recompiled image were classified. Six are inside sub_824961D0 itself;
   the others are sub_822396A8, sub_82496AE0, sub_824993A0, sub_82550460 and one
   stack-relative store that is unrelated. NOT ONE of them stores an immediate
   zero. There is no invalidation path to find because the title never wrote one.

WHICH MAKES THE FIELD A MEMO, NOT A CACHE-WITH-INVALIDATION. sub_824961D0's
prologue reads it and uses it when non-null, and the same function writes it six
times. The shape is 'if I already have one, use it; otherwise make one'. So the
title's invariant is that THE MEMOISED OBJECT OUTLIVES THE HOLDER'S USE OF IT,
and it holds that invariant by construction rather than by checking.

SO THE ANOMALY IS THE FREE, NOT THE READ. The correct question is no longer 'why
is the stale pointer not cleared' -- nothing was ever going to clear it -- but
'why is the rendering thread destroying an object the game thread has memoised'.
That is where this goes next, and it points back at the FDrawSceneCommand
realloc-to-zero already recorded on this entry, now with a reason to treat the
free as the wrong end rather than the read.

The 'nothing nulls that field' remark in the probe text was right about the fact
and wrong about its significance: it read as a missing mechanism, and it is
actually the design.

### Note (2026-07-30)
THE RUNS HAD NO CRASH REPORTER AT ALL, AND THREE OF MINE WERE EMPTY.

WORKFLOW DEFECT FIRST. Six consecutive runs of this repro dumped core and printed
NOTHING about the fault. Every hypothesis they were meant to test was judged
against a log that simply stopped, which is indistinguishable from a log with
nothing to say. Built runtime/fault_report.{h,cpp}: a SIGSEGV/SIGBUS handler that
names the signal, classifies the address against the guest mapping, and dumps the
host backtrace -- which IS the guest call chain, because recompiled functions are
host functions named sub_82xxxxxx. Chains to the previous disposition so the core
and exit status are unchanged. Six tests, including a FORKED one that faults on
purpose and asserts the report reaches the pipe, because a crash reporter is
otherwise only exercised by the crash it is needed for.

AND I WASTED THREE RUNS ON IT. The first version took the mapping as an argument
and was called with gears::Memory() at the top of main -- before SetMemory().
That killed the process on its first line. Three 280-second runs produced ONE
line each and I read conclusions out of them ('the handler is not displaced',
'the reporter did not fire') that were worth nothing. The log length is now the
first thing checked after every run. Install is split: armed with no mapping at
the first line of main, taught the mapping once memory exists.

WITH THE REPORTER WORKING, THE FAULT IS NAMED:

  signal SIGSEGV, address 0x7fbeb1c001fe, guest mapping 4 GiB at 0x7fbf33200000

The address is 2 GiB BELOW the mapping, so it is a HOST pointer, not guest
memory. That distinction is only trustworthy because the mapping base is printed
in the same run -- it is ASLR-randomised, and an earlier verdict of 'host
pointer' read against a different run's base would have been a guess.

The frame is __imp__sub_824961D0+0x14cb, and the chain above it is
sub_82495FB8 <- sub_8242A510 <- sub_8242B090 <- sub_8242AFF8 <- sub_821B4620 <-
sub_821B43F8 <- sub_82214480 <- sub_823CEAB8 <- sub_82428238 <- sub_822180F8 <-
sub_82218E10 <- sub_82218F98 <- _xstart. THE MAIN THREAD, not a render thread.

WHAT A HOST FAULT ADDRESS MEANS HERE. Recompiled code touches exactly two kinds
of host pointer: the memory base, and the function pointer it fetches from the
function table for an indirect call. A guest load through a stale pointer would
fault INSIDE the mapping. So this is an indirect CALL through a garbage guest
address: the table lookup returns rubbish and the jump lands on it. That is
consistent with the long-standing reading of a call through a stale vtable, and
it is the first time the fault itself has said so rather than being inferred.

NEXT: map +0x14cb inside sub_824961D0 back to its guest instruction, which names
the exact virtual call, and check what the function table does with an
out-of-range guest address -- if it indexes without a bounds check, that is a
runtime defect worth fixing on its own account regardless of why the guest
pointer was stale.

### Note (2026-07-30)
ROOT CAUSE PROVEN, WITH THE BYTES.

Every indirect call in the image (29,190 sites) now validates its target, so the
crash reports itself instead of jumping into host memory. The report:

  INDIRECT CALL TO 0xff, which is not a guest function
  lr 0x82496a6c, thread 'host' (the MAIN thread)
  r3=0x42ba0fc0  r24=0x41c84e00  r11=0xff
  object r3 0x42ba0fc0: 42ba1080 00000001 00000917 00000000 00000001 ...
  its first word (the vtable pointer) is 0x42ba1080
  vtable 0x42ba1080: 42ba1140 00000001 00000000 ... 00000001 000000ff ffffffe3

AND THE GUEST CODE AT THE SITE:

  lwz r11,1396(r24)      ; a guard beside the memo
  cmpwi cr6,r11,0
  bne cr6,0x82496a9c     ; NON-ZERO skips the call entirely
  lwz r3,1376(r24)       ; the memoised object
  lwz r11,0(r3)          ; its vtable
  lwz r11,52(r11)        ; slot 13
  bctrl

WHAT THE BYTES SAY. The 'vtable pointer' is 0x42ba1080 -- a HEAP address 0xc0
bytes past the object itself, not an image address, and what it points at is
plainly a list node with counters rather than a function table. Slot 13 of it is
0x000000ff, which is the bad target. So the block at r3 is no longer the object
the title memoised: it has been freed and REUSED as a different structure.

And 0x42ba0fc0 is verbatim the address the freed-while-cached probe reported in
an earlier run ('FREEING 0x42ba0fc0 WHILE IT IS STILL CACHED at holder
0x41cb4e00+1376, occurrence 132'). The same is true of the previous run's pair
(0x42babc40 at holder 0x41ca4e00). The probe and the fault agree on the address,
which is the first time these two lines of evidence have met.

SO: use-after-free of the object memoised at holder+1376, used from the MAIN
thread, freed from the render path, and by the time it is used the memory has
already been recycled into another data structure. That is no longer inferred
from a symptom -- the object, its false vtable, and the slot that produced the
bad target are all printed.

THE GUARD IS THE THING TO UNDERSTAND NEXT. holder+1396 gates this call: non-zero
means skip. It is written by sub_82496AE0, which also writes the memo at +1376 --
so one function establishes both, and sub_824961D0 consumes both. Either the port
fails to set the guard where the title would, or the guard means something other
than validity. That is the next question, and it is now a narrow one.

ALSO FIXED, AN INSTRUMENT THAT LIED: the first version of the register dump
bounded itself to the loaded image and reported of an out-of-image address 'that
alone makes it not an object'. False, and stated with confidence -- heap objects
live outside the image, and the object in this very crash is one. Bounded to the
guest mapping now.

### Note (2026-07-30)
A NAMED PORT-SIDE MECHANISM: THE GPU PIPELINE HAS NO DEPTH.

Reasoning pass (fable) proposed that the title's safety came from LATENCY the
console guaranteed, not from any invalidation protocol -- and that the port
collapses it. The port half of that is now CONFIRMED by reading the code, and it
is written in our own comments.

runtime/vd_null_gpu.cpp, EVENT_WRITE_SHD -- the fence write the guest uses to
learn that the GPU has retired work:

    // The event pipeline on hardware defers the write until the work retires;
    // with no work executed there is nothing to defer past, so it completes now.
    StoreEndian(data[1], data[2]);

And the ring read-pointer writeback, published INSIDE the consume loop, once per
packet:

    StoreGuest32(g_ringBuffer.readPtrWriteBackAddress, rptr);

So both signals a title can use to ask 'is the GPU past this point yet' are
answered YES essentially the instant the packet is parsed. On 360 both trail the
game thread -- typically a frame or two -- because they reflect real work
retiring.

WHY THAT WOULD PRODUCE EXACTLY THIS BUG. If the render-side deferred destruction
drain is gated on one of those values, then on console the free always executes
one or two frames AFTER the game thread enqueued the release and refreshed the
memo at +1376. Here the gate is satisfied immediately, the drain fires with zero
pipeline depth, and the free OVERTAKES the refresh it always trailed. That
predicts every observed fact at once: one identical drain stack ~130x per run, a
deterministic frame, the render thread freeing, the main thread using, no
invalidation anywhere in the image (none was ever needed), and the dead
bIsWriting/SuspendRendering seams (the title never needed them because the
latency contract did the work).

IT ALSO KILLS THE 'SAFE BY ALLOCATOR TIMING' READING, using bytes already
captured. The crashing block's word 0 held 0x42ba1080 -- a heap address 0xc0
further along, which is what a freelist link looks like. If the pool writes its
link into word 0, a stale vtable read after ANY free would crash on the console
too, recycling speed irrelevant. That means the console never performed a stale
read at all, and its ordering was genuinely intact. Being confirmed statically in
the free at 0x822158e4.

WHAT IS NOT YET ESTABLISHED, AND I AM NOT ACTING UNTIL IT IS. Nobody has shown
that the guest's free gate actually READS one of these values. Adding latency
before that is confirmed would be a delay-until-it-works bandaid, and it would
look like a fix. The outstanding question is which predicate the drain waits on:
walk up 0x82170470, 0x8232ae10, 0x825552a4, 0x82551240, 0x825552fc, 0x8250b284,
0x825550f4, 0x82444f7c, 0x82445038, 0x8243ae00 and name what it loads and
compares. If it reads the writeback address or a fence scratch word, the root
cause is named and the honest fix is to stop reporting GPU progress ahead of
where hardware could -- a platform contract, not a sleep.

ALSO STILL UNMEASURED: the dynamic history of the guard at +1396. The 11 writers
of +1376 were classified; the writers of +1396 never were. That is the one word
in this mechanism whose behaviour nobody has observed.

### Note (2026-07-30)
ROOT CAUSE FOUND, END TO END, AND IT IS NOT A RACE. THE OBJECT IS CREATED, DELETED AND USED INSIDE ONE CALL ON ONE THREAD.

THE CLASSES, read from the image, not inferred from the crash:
  sub_824961D0 = ULinkerLoad::CreateLoader().  r3/r24 = the ULinkerLoad.
    +0x0cc/+0x0d0  ULinker::Filename (FString)
    +0x14c         ULinker::LoadFlags (bit0 = LOAD_SeekFree)
    +0x0e4/+0x0ec  FArchive::ArVer / ArLicenseeVer (0x82496a20 stores 374)
    +0x560 (1376)  ULinkerLoad::Loader (FArchive*)  -- THE MEMO
    +0x574 (1396)  bHasSerializedPackageFileSummary -- THE GUARD
    +0x584/+0x588  bTimeLimitExceeded / IsTimeLimitExceededCallCount
  sub_82496AE0 = ULinkerLoad::SerializePackageFileSummary().
  sub_8242C098 = FArchiveAsync::FArchiveAsync(const TCHAR*).
  FArchive vtable slots: 1 Serialize, 12 Tell, 13 TotalSize (+0x34), 15 Seek,
  18 Precache (+0x48), 19 SetCompressionMap. Derived from sub_82496AE0's own
  use of them (Tell -> delete -> new FArchiveAsync -> Seek(Tell) ->
  SetCompressionMap is verbatim UE3's PKG_StoreCompressed fallback).

WHAT +1396 IS (the question this entry asked): bHasSerializedPackageFileSummary,
a BOOL. It is written in ONE place, 0x82496fbc `stw r24,0x574(r31)`, at the very
end of sub_82496AE0, and r24 there is the CONSTANT 1 (`li r24,1` at 0x82496cbc,
never reassigned). It is the early-out at the top of that same function
(0x82496afc). Meaning: "the package header has already been read through this
Loader". While it is 0, CreateLoader precaches the first 128 KB:
    82496a4c lwz r11,0x574(r24) ; bne -> skip
    82496a58 lwz r3,0x560(r24)  ; 82496a60 lwz r11,0x34(r11) ; TotalSize
    82496a6c cmpw r3,0x20000 / clamp ; 82496a88 lwz r11,0x48(r11) ; Precache(0,n)
CORRECTION TO AN EARLIER NOTE IN THIS ENTRY: "sub_82496AE0 stores a POINTER
(r24) rather than a flag, which suggests +1396 is a SELECTOR" is FALSE. r24 is
the literal 1. +1396 is not a validity bit, not an in-flight flag, and the port
is not missing any path that sets it -- in the crashing case the summary
genuinely has not been read yet, which is correct.

THE MECHANISM, every step measured or read:

 1. The checkpoint restore hands UGameEngine::PrepareMapChange (sub_82426D98)
    a one-element FName list whose entry is index 0 number 0 -- NAME_None.
      [linker] PrepareMapChange #1: 1 level name(s) at 0x42ee4880
      [linker]   level[0] FName index 0 number 0
 2. PrepareMapChange does FName::ToString (0x82426fa0) and at 0x82426fd4 calls
    UObject::LoadPackageAsync with it. There is NO FindPackageFile check on this
    path; the one at 0x82426f18 guards only the "<name>_LOC" companion load.
      [linker] LoadPackageAsync #1: ... len 5 -> 'None'
 3. In the async tick, sub_8242A830 asks the package cache and IGNORES THE
    ANSWER:
      8242a8c0 lwz r3,-0x7e20(r11)   ; GPackageFileCache (0x82BE81E0)
      8242a8c8 lwz r11,8(r11)        ; slot 2 = FindPackageFile
      8242a8d0 bctrl                 ; RESULT NEVER TESTED
      8242a8d8 lwz r10,0x54(r1)      ; out FString length
      8242a8e0 cmpwi cr6,r10,0
      8242a8f0 andi. r5,r11,0x81     ; LoadFlags = 0x81
      8242a8f4 bne cr6,0x8242a8fc
      8242a8f8 mr r4,r29             ; <- the EMPTY-STRING constant
      8242a900 bl 0x82495ea0         ; CreateLinkerAsync(parent, "", 0x81)
 4. So ULinkerLoad::Filename is "" and LoadFlags is 0x81 (LOAD_SeekFree set):
      [linker] ULinkerLoad 0x41c84e00: Filename '<empty>' (0 chars),
               LoadFlags 0x81, Loader(+1376) 0x42ba0cc0, +1396 0x0, ArVer 374
 5. CreateLoader's seek-free branch news an FArchiveAsync(""). Its ctor tail:
      8242c144 lwz r3,-0x7e44(r11)  ; GFileManager
      8242c14c lwz r11,0xc(r10)     ; slot 3 = FileSize
      8242c15c stw r3,0x78(r31)     ; FileSize
      8242c164 blt cr6,0x8242c174
      8242c168 stw r30,0x2c(r31)    ; r30=0 : ArIsError = FALSE
      8242c174 stw r28,0x2c(r31)    ; r28=1 : ArIsError = TRUE
    FileSize("") is -1, so ArIsError is set. MEASURED:
      [linker] FArchiveAsync #3 at 0x42ba0cc0 FAILED TO OPEN '' (FileSize -1)
      3 constructed, 1 with ArIsError.
 6. AND THE TITLE'S ERROR PATH IS ITSELF A USE-AFTER-FREE:
      82496520 bl 0x8242c098     ; new FArchiveAsync
      82496538 stw r3,0x560(r24) ; MEMOISED BEFORE THE ERROR IS CHECKED
      8249653c lwz r11,0x2c(r3)  ; ArIsError
      82496544 beq -> 0x824967e4 ; clean: carry on
      82496550 lwz r11,0(r3) ; li r4,1 ; 82496560 bctrl   ; DELETE IT
      82496564 ...Localize("Errors","OpenFailed")... report...
      82496620 b 0x824967e4      ; AND FALL THROUGH to the shared tail
    which is the tail at step 1 above: +1396 is 0, so it calls TotalSize()
    through the pointer it just deleted. MEASURED, same object, same run:
      THE CRASHING OBJECT 0x42ba0cc0 WAS DESTROYED with lr 0x82496564
      the memoised Loader: its most recent pool event was a FREE
      it entered CreateLoader 1 time(s); Loader was 0x0 at entry (CREATE path)
    0x82496564 is the return address of the bctrl at 0x82496560 -- CreateLoader's
    own open-failed branch. One call, one thread, no race.

WHY THE FAKE VTABLE LOOKS LIKE A LIST NODE. The pool writes its freelist link
into WORD 0 of the block it frees:
      82215480 stw r10,4(r4)     ; word1 := run length (1 for a single free)
      82215488 stw r10,0(r4)     ; word0 := old freelist head  <- THE LINK
      82215490 stw r4,0x10(r11)  ; head := this block (LIFO)
A 0xac request lands in the 192-byte size class (42 classes, "smallest >= n",
via the byte-granular table at pool+0x1138 built in sub_82215648), and
0x10000/192 = 341 blocks per page at stride 0xC0 -- which is exactly the
0x42ba0fc0 -> 0x42ba1080 -> 0x42ba1140 chain in the fault dump. So the "vtable"
was the freelist next pointer and slot 13 of it was another block's run counter.

WHY THIS IS SAFE ON THE CONSOLE: the branch is DEAD THERE. It runs only when a
package cannot be opened, and on a console with its disc the map is found. The
use-after-free is a real bug in the retail title's error path -- memoise, delete,
fall through -- that no console execution ever reaches. NOTHING IS MISSING FROM
THIS PORT'S SYNCHRONISATION. The port reaches it because the port drives the
title into async-loading NAME_None.

FIVE THINGS THIS RETIRES, so nobody re-walks them:
  - The cross-thread reading. The free is on the MAIN thread inside
    sub_824961D0. The render-thread FDrawSceneCommand chain is unrelated: it
    tears down FSceneRenderer and its view arrays unconditionally
    (0x825550f0 `bl 0x8250b228` is not guarded by anything), constantly
    recycling 192-byte-class blocks, which is why its addresses kept matching.
  - "156/179 frees of a still-cached block per run": mostly coincidental
    address matches from that recycling. The crash's own pair is the LAST
    occurrence and its holder matches r24 exactly; the rest do not.
  - The GPU/ring-latency hypothesis. All 12 frames of the render free chain were
    read: not one loads a value the host writes. The only cross-thread compare
    is the render-command ring at 0x82C0CB24, whose write pointer is advanced by
    the GAME thread under lwsync (0x824452f4, 0x82445470); the runtime's ring
    read-pointer writeback (vd_null_gpu.cpp) is a different object that no frame
    in the chain reads. The only external-global load in the chain, 0x82BECBA0
    at 0x825550c8, is a pointer stored into scene+0x98 and never compared.
  - "The cached value is a live block at ENTRY every time" was uninformative,
    not reassuring: a block re-issued to a different owner also has an
    allocation as its last event -- and in the crashing case Loader was 0 at
    entry, so that check never looked at the crashing pointer at all.
  - Option A from the decision note (port-level mutual exclusion around render
    Execute and the restore path) would have been wrong and would have hidden
    this. It is off the table.

THE CORRECT FIX IS UPSTREAM AND HAS NOTHING TO DO WITH LIFETIMES: make the
checkpoint restore produce the real map name so PrepareMapChange receives a
valid FName instead of None. SP_Prison_P.xxx IS PRESENT in the extraction
(scratch/game/WarGame/CookedXenon/SP_Prison_P.xxx), and chapter37.sav decodes to
"sp_prison_p", so the content is there and the name is simply never arriving.
That points straight back at this entry's own unresolved deserialisation: the
same runs still print
  FString deserialise #78278: archive array is populated
      (2147483648 bytes at 0x900006b0)
-- an impossible descriptor (0x80000000 elements at a non-guest address). The
FName comes out None because the FString it is built from comes out of that.
That is the next step, and it is a data-interpretation question, not a lifetime
one. DO NOT patch +1376, do not suppress the free, do not add a lock.

INSTRUMENTS ADDED (runtime/guest_probes.cpp, runtime/guest_indirect_call.cpp):
  - sub_8242C098 now reports ArIsError and FileSize per FArchiveAsync, with the
    construction count as its denominator and the stated blind spot that only
    LOAD_SeekFree packages reach that constructor.
  - sub_824961D0 records, per ULinkerLoad, the memo value AT ENTRY -- the single
    datum that separates "died between calls" from "died inside this call".
  - the bad-indirect-call reporter now dumps the ULinkerLoad behind the crash
    (Filename, LoadFlags, +1376, +1396, ArVer) when lr is inside sub_824961D0.
  - sub_8242C180's destruction ring gained a per-address map, so the destroying
    lr is found however long ago it happened.
  - sub_82426D98 / sub_8242AC78 report the level-name list and the async load
    request, which is what named NAME_None.
  Three previously DEAD instruments (ReportArchiveLifetime, ReportLastFree,
  ReportHolderSeam were defined and never called) are now wired to the abort
  path, which is the only exit this repro reaches.

### Note (2026-07-30)
THE WHOLE CHAIN, END TO END, AND IT STARTS AT THE FSTRING DESERIALISE.

Traced from the crash back to the data. Every link measured or read out of the
guest code, not inferred.

  1. FString deserialise from the checkpoint archive returns EMPTY.
     sub_8232B548 is FArchive& operator<<(FArchive&, FString&): r28 is the
     archive (the load/save flag is at +20), the length is negated for UTF-16,
     and there is a scan for characters above 255 to choose the encoding. It is
     called three times in a row in sub_821B4620 filling FStrings at r1+200,
     r1+184 and r1+136.

  2. sub_821B4620 then picks a literal because the FString is empty:
       lwz r11,140(r1)   ; the FString length -- ZERO
       lwz r30,136(r1)   ; its data pointer -- also zero
       mr  r4,r30 / mr r4,r28    ; empty length selects the constant instead
     Nothing writes 136(r1) or 140(r1) between the zero-init block at line 655
     and the read, except that deserialise. So the emptiness comes from step 1.

  3. FName construction gets the empty string. MEASURED, with its denominator:
       FName for the map change #1: string at 0x820e417c = '' (0 chars, EMPTY
       STRING), find-mode r5=128 r7=1 -> INDEX 0x0  <- NAME_None
       map-name probe: 1 FName construction(s) from the map-change site out of
       60608 in the whole run
     0x820e417c is in the IMAGE, so it is the empty-string literal, which
     confirms the branch taken in step 2 rather than merely being consistent
     with it. The 1-of-60608 is what makes the single line trustworthy: the
     return-address filter caught exactly one call and reported the total it
     filtered against.

  4. PrepareMapChange receives that: 'level[0] FName index 0 number 0'.

  5. LoadPackageAsync requests 'None' (len 5). The package cache cannot find it,
     the result is discarded, and an empty filename reaches CreateLinkerAsync.

  6. FArchiveAsync('') -> GFileManager->FileSize returns -1 -> ArIsError set.

  7. ULinkerLoad::CreateLoader (sub_824961D0) has ALREADY memoised the loader at
     +1376 before testing ArIsError, deletes it on the error branch, falls
     through to the shared tail, and calls slot 13 (TotalSize) through the
     deleted pointer. That is a genuine retail bug on a path a console with its
     disc never executes.

SO THE FIX IS AT STEP 1, and nowhere else. Everything from step 2 down is the
title behaving exactly as it would on hardware given an empty name. There is
nothing to fix in the lifetime, the pool, the threading or the GPU pipeline --
all four of which this entry has chased at length.

THIS ALSO EXPLAINS THE OLD ANOMALY nobody could place: the runs print 'FString
deserialise #78278: archive array is populated (2147483648 bytes at 0x900006b0)'.
2147483648 is 0x80000000 -- an int32 sign bit, i.e. a length field read as
INT_MIN. A negative length means UTF-16 in this format, so a garbage length is
read as an enormous wide string. That is the same deserialise, and it is the next
thing to fix.

WHAT WAS WRONG BEFORE, recorded so it is not re-derived: the cross-thread race
reading, the GPU-latency reading, the 'holder is freed' reading and the 'safe by
allocator timing' reading were all wrong. The 156 freed-while-cached events were
mostly coincidental address matches, because the pool recycles 192-byte-class
blocks constantly and word 0 of a freed block holds the freelist link -- which is
also why the 'vtable chain' at stride 0xC0 looked like a vtable and was not.

### Note (2026-07-30)
THE ARCHIVE IS EMPTY, AND THE OLD 'IMPOSSIBLE DESCRIPTOR' WAS A MISREAD.

Measured at the map-name deserialise, all in one run:

  the carrier holds 385 bytes at 0x42f501c0   <- correct, this is the save blob
  the archive's byte array is at 0x41dd0de4: data 0x0, count 0   <- EMPTY
  archive 0x40102020 fields: +0=0x8209e110 +4=0x176 +8=0x917 +16=0x1 +20=0x0
    ... +96=0xffffffff +108=0x2775 +112=0x41dd0de4 +116=0xb0800000

  map-name FString deserialise #1: -> length 10089
  map-name FString deserialise #2: -> length 0
  map-name FString deserialise #3: -> length 0
  3 deserialise(s) from the map-change site out of 78281 in the run

SO: the archive is in a valid loading state (+16=1 ArIsLoading, +20=0 not saving,
+4=0x176 = ArVer 374, matching the ULinkerLoad), and its byte array is EMPTY. The
385-byte carrier never reached it. Every read therefore copies nothing, which is
why the first read reports a length of 10089 -- that is uninitialised stack
garbage left in the length variable because Serialize wrote nothing over it -- and
why the two after it read 0 once ArIsError is set.

A CORRECTION THAT MATTERS. This entry has carried 'FString deserialise #78278:
archive array is populated (2147483648 bytes at 0x900006b0)' as an unexplained
anomaly for a long time, and the previous note here treated 0x80000000 as a
length field read as INT_MIN. BOTH WERE WRONG. +112 is a REFERENCE -- an
FMemoryReader holds a CONST REFERENCE to a TArray of BYTE -- so the descriptor is one
indirection further out. The old probe read +112 and +116 as data and count when
they are a reference and an unrelated field, and 0xb0800000 at +116 is the value
this project already documents as what physical page 0 holds. There was never an
impossible descriptor. The array is simply empty, and it always was.

That also retires the 'garbage length' reading in the note directly above this
one. The length is garbage, but as a CONSEQUENCE of an empty array, not as data
read out of the file.

WHERE THE FIX IS. sub_821B4620 copies the global carrier at 0x82BFB36C into
object+420 before deserialising (0x821B4938: dest r17+420, src 0x82BFB36C), and
the archive is constructed over that copy. The carrier is populated AT THE TIME OF
THE DESERIALISE -- just measured, 385 bytes -- so the copy either never runs, or
runs EARLIER, while the carrier is still empty. An ordering problem, and a
testable one.

NEXT: instrument the copy at 0x821B4938 -- does it execute, when, and what is the
carrier's count at that instant? Three outcomes, all distinguishable: it never
runs (a branch we do not take), it runs before the Kismet op fills the carrier (an
ordering bug, ours or the title's), or it runs with a populated carrier and the
copy itself is wrong.

### Note (2026-07-30)
THE CARRIER COPY IS GATED ON A VIRTUAL CALL, AND THE GATE IS TAKEN THE WRONG WAY.

Read out of sub_821B4620 (guest 0x821B4910..0x821B4944):

    lwz   r17,1496(r3)        ; r3 from sub_8218DED8(r30)
    lwz   r11,0(r17)          ; r17's vtable
    addi  r4,r1,112           ; an OUT parameter
    mr    r3,r17
    lwz   r11,656(r11)        ; SLOT 164
    mtctr r11
    bctrl                     ; -> r3
    lis   r11,-32064
    cmpwi cr6,r3,0
    addi  r29,r11,-19604      ; r29 = 0x82BFB36C, the carrier
    bne   cr6,0x821b4944      ; NON-ZERO -> SKIP the copy
    addi  r3,r17,420          ; dest = object+420
    mr    r4,r29              ; src = the carrier
    bl    0x821b5c90          ; the copy

So object+420 -- the array the checkpoint archive is constructed over -- is filled
from the global carrier ONLY when that virtual call returns ZERO. Measured
consequence: the archive's array is data 0x0 count 0 while the carrier holds the
correct 385 bytes, which means the call returned NON-ZERO and the copy was
skipped. The call claims to have supplied the data through its out-parameter at
r1+112 and does not.

This is the first link in the chain that could plausibly be OURS rather than the
title's. Everything downstream -- empty array, garbage length, empty FString,
NAME_None, LoadPackageAsync('None'), FArchiveAsync FileSize -1, the retail
use-after-free in CreateLoader -- follows correctly from an empty object+420.

WHAT IS NOT YET KNOWN, and I am not guessing at it: what class r17 is, what slot
164 resolves to, whether that target is guest code or one of our native
overrides, and what it does with the out-parameter. If it is a native override
returning a success code without filling the buffer, that is the bug and it is a
one-line class of mistake. If it is guest code, then the predicate it evaluates is
the next question and the fault is further upstream still.

NEXT: instrument the call. Report the resolved target address, whether it is in
the recompiled image or an override, the return value, and the out-parameter's
contents before and after. All four, because 'it returned non-zero' alone does not
distinguish a wrong override from a correct refusal.

ALSO REPAIRED: the note above this one had a sentence mangled by zsh command
substitution -- backticks inside a double-quoted argument to catalog.py note ran
as a command and left a hole. Use single quotes or avoid backticks in note text.

### Note (2026-07-30)
THE GATE RETURNS SUCCESS HAVING LOADED NOTHING, AND THE LITERAL IS A FILENAME.

Hooked sub_821B94D8 -- the function whose return value decides whether the global
carrier is copied into the array the checkpoint archive is built over. Two calls
per run, with opposite outcomes:

  carrier gate #1: object 0x434fc240, out-param count 14 (other path),
                   literal "default_checkpoint.sav"
    object+420 before: data 0x0 count 0 -> after: data 0x4120b400 count 385
    RETURNED 0x1
    the carrier meanwhile holds 0 bytes at 0x0

  carrier gate #2: object 0x41dd0c40, out-param count 0 (literal path),
                   literal "default_checkpoint.sav"
    object+420 before: data 0x0 count 0 -> after: data 0x0 count 0
    RETURNED 0x1
    the carrier meanwhile holds 385 bytes at 0x42f51c00

CALL #2 IS THE ONE THAT MATTERS: 0x41dd0c40 + 420 = 0x41dd0de4, which is exactly
the archive byte-array address measured in the previous run. So the object whose
array stays empty is the object the checkpoint archive is constructed over.

WHAT THIS ESTABLISHES:

  * The save data loads FINE through the other path -- 385 bytes, the right size,
    into call #1s object. So reading the save is not broken.
  * Call #2 takes the LITERAL path because its out-parameter count is 0, meaning
    the caller supplied no name and the title falls back to a hardcoded
    "default_checkpoint.sav".
  * That literal load populates NOTHING, and the function still RETURNS 1.
  * Because it returns non-zero, sub_821B4620 skips the carrier copy, so the
    385 bytes sitting in the carrier at that very moment never reach the array.

A CORRECTION TO MY OWN REASONING recorded so it is not repeated: I inferred from
the disassembly that the return value is derived from object+420 via sub_8232F010
and therefore reports whether the array is populated. That inference is WRONG --
it returns 1 with an empty array. Whatever sub_8232F010 computes, it is not
emptiness. The probes conclusion text for call #1 is also mis-worded for the same
reason: it says the later emptiness must mean something CLEARS the array, when in
fact there are simply TWO DIFFERENT OBJECTS and only one of them was ever filled.

THE OPEN QUESTION, and it is now narrow and probably OURS: why does the literal
load fail? The literal is a FILENAME, and this runtime stores that content as a
DIRECTORY named default_checkpoint_sav containing a file the title created as
save:\\Pla. If sub_821B5F30 opens "default_checkpoint.sav" as a path and our
filesystem exposes it under a different name, the open fails on the port and would
have succeeded on the console. That is a port-side naming mismatch, which is
exactly the class of defect worth fixing properly.

NEXT: read sub_821B5F30 and find what path it actually opens, then compare against
what guest_filesystem exposes. Report the attempted path verbatim -- not "the open
failed", which does not say which name was tried.

### Note (2026-07-30)
THE LITERAL PATH NEVER TOUCHES THE FILESYSTEM, AND THE WRITE/READ NAMES DISAGREE.

Sequence around gate #2, from one run, in order:

  [xam] XamContentCreateEx("default_checkpoint_sav") disposition 3, content
        exists -> open
  [fs]  mounted "save:" -> .../default_checkpoint_sav (writable)
  [xam] content "default_checkpoint_sav" mounted as "save:" (existing)
  carrier gate #2 ... literal "default_checkpoint.sav"
    object+420 before: data 0x0 count 0 -> after: data 0x0 count 0; RETURNED 0x1

So the content mount SUCCEEDS immediately before the gate, and then the literal
load performs NO FILE I/O WHATSOEVER -- there is not a single [fs] line between
the mount and the gate returning. It fails before reaching the filesystem, so
this is not an open that returned an error; nothing was ever opened.

AND THE TWO NAMES DISAGREE. The title WRITES its checkpoint to save:\\Pla -- a
name derived from the gamertag, truncated to four bytes including the terminator
by CopyGamertag, and that file is the 385-byte blob that decodes byte-identically
to chapter37.sav. The loader is looking for "default_checkpoint.sav". One of those
two is wrong and they cannot both be right, but WHICH is wrong is not established
and I am not guessing: it could be that the write name should be the literal, or
that the literal path is a fallback that is never meant to succeed and the real
failure is the empty out-parameter that sent it down there.

THAT EMPTY OUT-PARAMETER IS THE BETTER SUSPECT. Gate #1 succeeds with an
out-parameter count of 14 and loads 385 bytes; gate #2 fails with a count of 0.
The paths differ only in that. So the question worth asking first is not "why did
the literal load fail" but "why did the caller supply no name the second time,
when it supplied a 14-character one the first time". A 14-character name is about
the length of a checkpoint filename.

NEXT, in this order:
  1. Read sub_821B5F30 and establish what it reads FROM -- it is not a file open,
     so it is either a content/device API or a memory source.
  2. Find who fills the out-parameter for gate #1 and why it is empty for gate #2.
  3. Only then decide whether save:\\Pla is the wrong write name. Fixing the name
     first would be a guess that might paper over the real ordering problem.

### Note (2026-07-30)
THE NAMES, AND A METHOD ERROR OF MINE.

MEASURED, both gates, with the out-parameter contents rather than just its count:

  carrier gate #1: out-param data 0x433f35c0 count 14 = "chapter37.sav"
                   (other path) -> object+420 gets 385 bytes, RETURNED 1
  carrier gate #2: out-param data 0x0 count 0 = "" (literal path)
                   -> object+420 stays empty, RETURNED 1

And with the fs debug channel enabled, gate #1s data source is named:

  [fs] open "D:\\WarGame\\Checkpoints\\chapter37.sav" -> handle 0xf4000080
  [fs] read 385 bytes at 0 -> 00 00 00 02 00 00 00 25

So the 385 bytes are the games own BUNDLED DEFAULT CHECKPOINT off the disc, not a
user save. The file on disk after a run confirms the content is right: 385 bytes,
version 2, chapter 0x25, FString length 12 "sp_prison_p".

A METHOD ERROR TO RECORD, because it is the same one this project keeps making.
The previous note asserted "the literal path performs NO FILE I/O WHATSOEVER --
there is not a single [fs] line". That was true of the log and NOT evidence,
because failed opens are logged at kernel_file.cpp:151 through
lucent::debug("fs", ...) -- a channel that is OFF by default. The absence I
reasoned from was an absence of LOGGING. Re-run with GEARS_LUCENT_DEBUG=fs and a
positive control (40 opens logged, including "not found" ones for the startup
movies), and the conclusion does hold -- default_checkpoint.sav is attempted
nowhere, its only 10 mentions being my own probe lines -- but I reached a right
answer by an unsound route and would not have noticed had it been wrong.

A STRUCTURAL FACT I HAD WRONG: the discovery hook keyed on lr == 0x821B4928 fired
ONCE while the gate ran TWICE. So gate #1 is called from a DIFFERENT call site
entirely, and only gate #2 is the one inside sub_821B4620. They are not two
iterations of one loop; they are two different callers, on two different objects,
and only one of them ever supplies a name.

AND A CANDIDATE READING OF THE RETURN VALUE, not yet confirmed. In the tail, r31
holds the object pointer from "mr r31,r3" at entry. The non-literal path
overwrites it with the result of sub_8232F010; the literal path branches to
0x821b9588, which appears to be PAST that overwrite, so it would return the OBJECT
POINTER -- non-zero by construction, regardless of whether anything loaded. If
that is right, "returns 1" on the literal path carries no information at all and
the caller is not being lied to so much as reading a value that was never a status.
It also means the console behaves identically here, so the literal load must
SUCCEED on hardware, and the whole question collapses to: what does sub_821B5F30
read from, and why does it read nothing here?

That last question is the only one left, and it is being worked.

### Note (2026-07-30)
CORRECTION: THE RETURN VALUE IS sub_821B5F30 OWN RESULT, AND IT CLAIMS SUCCESS.

The previous note floated a reading in which the literal path branches PAST the
instruction that overwrites the return register, so the function would return the
object pointer and carry no status. I flagged it as unconfirmed. IT IS WRONG.

Computed the addresses exactly (sub_821B94D8 starts at 0x821B94D8, four bytes per
instruction):

  0x821b9578 [40] li r31,0
  0x821b957c [41] b 0x821b958c
  0x821b9580 [42] addi r3,r31,420
  0x821b9584 [43] bl 0x8232f010
  0x821b9588 [44] mr r31,r3        <- the literal path branches HERE
  0x821b958c [45] addi r3,r1,80
  0x821b9590 [46] bl 0x821fc470
  0x821b9594 [47] mr r3,r31

The literal path branch at instruction 18 targets 0x821b9588, which IS the
overwrite. So r31 does get replaced, and r3 at that moment is whatever
bl 0x821b5f30 (instruction 17) returned. The three cases are therefore:

  literal path      : return sub_821B5F30(name, object+420)
  other path, fail  : return 0            (li r31,0 at instruction 40)
  other path, ok    : return sub_8232F010(object+420)

CONSEQUENCE: sub_821B5F30 RETURNS NON-ZERO WHILE POPULATING NOTHING. That is a
real claim of success by a real function, not an artefact of register reuse, and it
is the last unexplained link. There is no longer any reading in which the return
value is meaningless.

ALSO CLEANED UP: the temporary discovery hook in guest_indirect_call.h -- a compare
against one return address on the hottest path in the image, 29,190 call sites --
has been removed now that it has named its target, as its own comment promised. It
did two useful things before going: it identified sub_821B94D8, and it revealed
that it fired ONCE while the gate ran TWICE, which is how the two-call-sites fact
was found. Both are recorded above, so the hook has no reason to stay.
