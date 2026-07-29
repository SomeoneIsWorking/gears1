---
id: 45
title: The title asks for 2.6 GB immediately after mounting its save content
status: open
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
