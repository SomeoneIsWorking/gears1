---
id: 20
title: Every vertex shader in this title has a zero vfetch stride; Xenia asserts on it
status: resolved
symptom: xenos_translate aborts (SIGTRAP) on every vertex shader; assert_not_zero(fetch_instr.attributes.stride)
tags: gpu,shaders,xenia,vertex
created: 2026-07-23
updated: 2026-07-23
---

Symptom: all 103 distinct vertex shaders abort the translator, all 322 pixel
shaders translate fine. Backtrace on every one of the 103 lands at the SAME
place: `xe::gpu::Shader::GatherVertexFetchInformation`,
`extern/xenia/src/xenia/gpu/shader_translator.cc:421`,
`assert_not_zero(fetch_instr.attributes.stride)`.

Not a bad container parse, and this was checked rather than assumed:

- With the assert compiled out, the microcode disassembles cleanly on both
  sides of the fetch (e.g. `vfetch_full r1, r0.x, vf0` / `alloc position` /
  `max oPos, r1, r1` / eight interpolator `add`s -- a coherent UI vertex
  shader), and the resulting SPIR-V passes `spirv-val`.
- Decoding the raw instruction words directly (stride is the low 8 bits of the
  third dword of a `VertexFetchInstruction`) gives `opcode=0 stride=0
  offset=0`. The zero is really in the game's bytes.

So the assert encodes an assumption -- that the shader carries its own vertex
stride -- which Gears of War does not satisfy. Xenia's own comment right above
it ("It may not hold that all strides are equal, but I hope it does") flags the
same assumption.

Consequence for the port, and it is a real one: **the HLE layer must supply the
vertex stride from the vertex fetch constant at bind time**, because the shader
never states it. Do not paper over this later by inventing a stride.

Xenia's asserts are therefore off by default in `xenia_gpu/`; restore them with
`cmake -DGEARS_XENIA_ASSERTS=ON` when investigating a translation problem.
