---
id: 22
title: The title patches vertex-fetch instructions into the microcode at bind time; the container's zero stride is never executed
status: resolved
symptom: captured microcode does not match the offline shader corpus; vertex shader stride is zero in the container
tags: gpu,shaders,vertex,xenia,binding
created: 2026-07-23
updated: 2026-07-23
---

Entry #20 recorded that every vertex shader in the cooked packages leaves the
`vfetch_full` stride field zero, and inferred that the HLE layer would have to
supply the stride from the vertex fetch constant at bind time. That inference is
now confirmed directly, and the mechanism is the title's own D3D, not something
the port has to invent.

Evidence: microcode captured from the PM4 sequencer loads the title actually
issues (GEARS_SHADER_CAPTURE=1) was compared against the offline container
corpus (scratch/shaders/all, 425 containers).

- Of 18 distinct vertex-shader payloads bound at runtime, **every single
  `vfetch_full` carries a non-zero stride** (observed 2, 3, 4, 5, 7, 8, 9, 10,
  12, 16, 22). `Stride=0` does not occur once. In the offline corpus it is zero
  everywhere.
- 12 of the 18 have an offline container of identical length whose microcode
  differs from the captured version **only in fetch instructions** -- compared
  as disassembly, every differing line on both sides is a `vfetch`. The ALU body
  is byte-identical.
- At dword level the patch rewrites the whole `vfetch_full` instruction, not
  just the stride byte: e.g. word1 0x00000688 -> 0x00393A88 and word2
  0x00000000 -> 0x00000003. That is the fetch constant being merged in.

Consequence, and it changes the shape of the shader work:

- The offline corpus is a corpus of **templates**. It is not what runs.
- A shader must be translated **after** the fetch patch, i.e. keyed on the
  post-patch microcode, or the vertex inputs will be wrong. Capturing at
  IM_LOAD/IM_LOAD_IMMEDIATE gets this right for free; capturing at
  D3DDevice_SetVertexShader (sub_82222B98) would not.
- Xenia's `assert_not_zero(fetch_instr.attributes.stride)` is correct about
  hardware after all -- it only fired because we fed it unpatched template
  microcode. It is still off by default in xenia_gpu/ so the offline corpus can
  be measured; runtime-captured microcode does not need it off.

Not established: whether the pixel-shader side has an analogous patch. Nine
captured pixel shaders are byte-identical to corpus entries, so at least those
are not patched; several others differ from their nearest corpus entry in ALU
operands, which is a different shader rather than a patched one.
