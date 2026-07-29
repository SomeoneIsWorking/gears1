---
id: 45
title: The title asks for 2.6 GB immediately after mounting its save content
status: resolved
symptom: out of guest heap 0x40000000: wanted 0x9f000000 bytes, right after 'content default_checkpoint_sav mounted as save:'; the allocation fails and the title carries on
tags: saves,content,memory,port
created: 2026-07-29
updated: 2026-07-29
---

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
