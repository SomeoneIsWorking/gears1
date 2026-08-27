---
id: I005
kind: instrument
status: trusted
created: 2026-08-27
---

## Instrument

GEARS_NATIVE_RHI_OBSERVE ordered semantic draw/binding/present comparator

## Validated by

Focused tests feed matching and deliberately altered draw packets, binding
state, and present packets, observe both answers, and prove one global sequence
across event kinds. A
headless menu walk through frame 1712 produced 90,854 draw matches with zero
missing or mismatched packets; a later headless run through frame 120 produced
970 texture/shader binding matches with zero missing or mismatched state. After
adding the terminal present event, a headless run through frame 240 matched
236/236 draws, 1,914/1,914 bindings, and 240/240 present packets with no missing
or mismatched observation. After adding transient mapped-buffer ranges, focused
controls demonstrated the missing-resource, wrong-address, and wrong-size
answers. A headless menu walk through frame 1980 then matched 153,214/153,214
draws (including 28,550 transient-vertex and 102,353 transient-indexed range
pairs), 430,563/430,563 bindings, and 1,980/1,980 presents with no missing or
mismatched observation.

The first label assigned to `0x8222B068`, vertex stream, was falsified by the
retained body and a live debugger sample. The function writes one of four
colour-target object slots and its interleaved descriptor; `0x8222B398` writes
the adjacent depth-target object slot and two depth descriptor words. A fast
headless run through frame 780 matched 6,481 colour-target and 3,555
depth-target updates, with zero errors across 83,573 bindings. The colour
descriptor arm first demonstrated its mismatch answer live: comparing the
pre-call object word failed because the retained body conditionally normalizes
four formats. The corrected arm compares the post-call normalized object word
with the separate device descriptor shadow.

The bound-index arm is validated against both outcomes. Focused controls alter
the DMA address, element width, and evidence presence and are rejected. A live
headless run through frame 780 matched 3,717 bound-index draws against the
packet's independently decoded address, byte length, 16/32-bit width, and
endian mode. It also matched 3,926 index-buffer bindings, including the full
object-derived allocation view, with zero missing or mismatched observations
among 24,233 draws and 80,023 bindings.

The actual vertex-stream binder is `0x8222AE20`. Its retained body and callers
independently establish 16 slots carrying an object, offset-adjusted fetch
address, remaining byte range, and dword-encoded stride. Focused controls reject
an altered stride and invalid source ranges. A live headless run through frame
780 matched all 2,463 vertex-stream updates with zero missing or mismatched
observations among 86,002 total bindings.

The first bound-draw comparison validated the instrument's mismatch answer live:
an 18-slot snapshot reported a second stream at slot 17 with object
`0x09000000` or `0x0A000000`, while the ordered setter state contained only
slot 0. Static reinspection showed that the reset loop stops at slot 16 and
that the supposed slot-17 object overlaps the stride-byte table. Restricting
the independent snapshot to the proven 16-slot contract removes the false
resource rather than suppressing the mismatch.

With the corrected contract, a headless run through frame 780 matched all
3,715 bound-index draw snapshots against their ordered active vertex views. In
the same run all 24,239 draws, 86,061 bindings, and 780 presents matched, with
zero missing observations and zero mismatches. Focused tests separately force
a changed stride, a missing snapshot, and a different stream count.

The render-target identity arm likewise demonstrated both answers. Persisting
bind-time descriptors produced 3,141 draw mismatches by frame 630 while object
identities still agreed; one repeated transition was `0x302D0` to `0xC02D0`
without another target bind. After restricting binding state to its proven
ownership, a headless run through frame 600 matched ordered active color/depth
objects for all 3,786 draws, including 482 bound-index draws, with zero missing
or mismatched observations across 15,698 bindings and 600 presents.

## Known failure modes

Colour-target descriptors are setter outputs, not always verbatim input object
fields. Capture the normalized post-call word; using `object+0x1C` before the
call produces false mismatches for four device-mode-dependent formats.

The DMA size field counts 16-bit units for both index widths. Treating it as a
dword count doubles every 16-bit consumed range and cannot match the semantic
slice.
