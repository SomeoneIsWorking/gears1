---
id: I005
kind: instrument
status: trusted
created: 2026-08-05
---

## Instrument

tools/ucode_reduce.py (symbolic register-file reduction of a Xenos shader)

## Validated by

**The first validation of this tool was made against a version with a
dropped-instruction bug, and is superseded.** That version treated a BARE
destination (`mul r9, r3.xyyz, c255.xyzx` -- all four components, no dot, no
mask) as writing nothing, so every later read of that register silently fell back
to its input value and the tool printed a tidy, wrong program without refusing.
It was caught on ps_d99a15450a08043a, where `r9` appeared in the output as an
input register although the shader plainly computes it.

The validation that stands, after the fix: the tool's reduction of
ps_1f1a3f779667a02a is **algebraically identical** to the independent throwaway
simulation that `runtime/shaders/base_pass_lightmap.frag` was actually written
from -- 38,478 normalised characters per colour channel, exact match on all three,
compared by fully expanding both to their leaves. That shader is bit-exact against
the translated shader on two captures, so the reduction it came from is correct
and this tool reproduces it.

`--selftest` runs seven cases, four of them things it must get NEGATIVE or
defensive about: a pair of rotations that compose to the identity must leave
`oC0.x` depending on `r1.x` and NOT on `r1.y` or `r1.z`; a bare destination must
write all four components AND not then be reported read-before-written; and a
shader containing `loop` must be REFUSED. The suite has now caught two real
errors -- a wrong expectation of mine about which synthetic rotation cancels, and
the bare-destination bug above.

Every run prints the registers **read before ever being written**: those must all
be interpolators, and any computed register appearing there means an instruction
was dropped. That report exists because it is the tell the first validation
missed.

Blind spots, printed with every run: no control flow, no predication, no
memexport, and none of the runtime's fetch or output epilogue (texture sign
decode, integer scale, exponent bias, gamma encode).

## Known failure modes

(none recorded yet)
