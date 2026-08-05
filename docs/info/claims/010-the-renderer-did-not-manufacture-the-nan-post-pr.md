---
id: C010
kind: claim
status: holds
created: 2026-08-05
tags: gpu,guest,constants
depends: runtime/frame_capture.cpp, tools/find_const_block.py
---

## Claim

The renderer did not manufacture the NaN post-process constants that black out play_v2.gfr: they were already in the guest's memory

## Evidence

tools/find_const_block.py finds c7=(0,0,0,0.5)||c8=(ffc00000 x3, 0) seven times in play_v2.gfr's guest memory (guest 0x1d1b0, 0x73aa0, 0xe5830, 0x1576a0, 0x1c9530, 0x23b4a0, 0x2ad430) and nowhere in courtyard.gfr or act1_v2.gfr, both of which render and both of which carry the working pattern instead

## What would falsify it

if the capture writer itself can introduce those bytes -- e.g. by dumping the packed constant blocks into the guest window it stores -- then the hits are our own output read back and prove nothing about the guest

Checked when the claim was made, because it is the one thing that would make the
whole result circular: `WriteFrameCapture` only READS `in.guestBase` (it scans
for non-zero blocks and copies them out), and `PackFloatConstants` writes the
packed blocks into a `std::vector` that reaches the GPU through the frame arena,
never into the guest window. Re-check this if either ever gains a write path
into guest memory -- which is why both files are named in `depends`.

The hits are also inside the capture's guest-memory blocks specifically, not in
its register-snapshot or microcode sections: all seven mapped through the block
table to a guest address, and the tool prints an unmapped hit as a bare offset
precisely so the two cannot be confused.
