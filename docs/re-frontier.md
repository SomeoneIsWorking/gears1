# RE Frontier — the ordered RE dependency chain toward a faithful BL2

Tracked by `tools/re_frontier.py` (consult it FIRST; update it in the SAME commit
that changes a step). This is the fine-grained companion to `docs/codemap.md`:
the codemap says *what subsystem exists*, this says *which ordered RE step is
real reverse-engineering vs a hack that jumped ahead*.

**Hard rule (no hacks / no fallbacks):** a `⛔ hack` status is DEBT, never an
acceptable resting state. It marks a shortcut standing in for absent RE and MUST
be removed as its real mechanism lands. `re_frontier.py hacks` is the debt list;
`re_frontier.py next` tells you the next RE-ready step.

**`re-verified` MEANS FAITHFUL to the real target — not "the mechanism runs."** A
step is `re-verified` only when its OUTPUT matches the real game/binary (look /
sound / behavior) on real data. An internal trace ("bytecode reached the call
site", "N rows attached") is a mechanism check, NOT faithfulness — if it runs but
the result doesn't match the real target, it is `re-partial` with the
faithfulness gap named. The user observes the running system; that observation
overrides any internal trace.

**Fail fast & loud:** a failure must surface loudly, never silently fall back —
unless the fallback IS intended behavior of the real target being reproduced.

Statuses: ✅ re-verified · 🟡 re-partial (honest gap) · 🔬 in-progress ·
⛔ hack (debt, must remove) · ⬜ todo · ➖ skip-by-design · ⏸ blocked (computed).

<!-- Machine-edited by tools/re_frontier.py add/set. Format: `## <area>` sections;
     each entry is `### <id> — <title>` followed by `- <field>: <value>` lines. -->

## core

### area.first-step — Describe the first RE step in this chain
- status: todo
- deps: 
- evidence: 
- where: 
- gap: Fill in real steps; add deps to encode the RE dependency order.
- notes: 


## gpu

### cmd-processor — PM4 command processor executes the ring
- status: re-verified
- deps: 
- evidence: runtime/vd_null_gpu.cpp; executes TYPE0/IB/DRAW_INDX/fences/predication/IM_LOAD; verified 30fps 20000+ frames
- where: runtime/vd_null_gpu.cpp
- gap: 
- notes: 

### shader-xlate — Xenos microcode -> SPIR-V translation
- status: re-verified
- deps: cmd-processor
- evidence: xenia_gpu/ island; 38/38 runtime-bound shaders pass spirv-val, reverified independently
- where: xenia_gpu/, tools/xenos_translate
- gap: 
- notes: 

### shader-capture — Capture runtime-bound shaders at IM_LOAD
- status: re-verified
- deps: cmd-processor
- evidence: catalog #21; 38 distinct payloads, stable across runs, captured at PM4 IM_LOAD
- where: runtime/vd_null_gpu.cpp
- gap: 
- notes: 

### present — Host swapchain present driven by guest VdSwap
- status: re-verified
- deps: cmd-processor
- evidence: runtime/gpu_present.cpp; present 1:1 with guest swap over 8190 frames; headless falls back
- where: runtime/gpu_present.cpp
- gap: 
- notes: 

### const-capture — Track SET_CONSTANT: ALU-float/fetch/bool-loop constant files
- status: todo
- deps: cmd-processor
- evidence: 
- where: runtime/vd_null_gpu.cpp ~798
- gap: SET_CONSTANT (0x2D) currently falls through default: and is skipped; the UBO bytes the shaders need are not captured. Verifiable by dumping and comparing against Xenia.
- notes: 

### draw-params — Detect hot pair at DRAW_INDX and capture draw params
- status: todo
- deps: shader-capture, const-capture
- evidence: 
- where: 
- gap: primitive type, index count/format/address or inline indices from packet + register state
- notes: 

### system-constants — Port Xenia SystemConstants + UpdateSystemConstantValues
- status: todo
- deps: const-capture
- evidence: 
- where: 
- gap: 29-field struct, ~257 lines register->UBO derivation in vulkan_command_processor.cc; NDC/index-endian placement source of truth
- notes: 

### draw-backend — First real draw: hot pair geometry into swapchain
- status: todo
- deps: shader-xlate, present, draw-params, system-constants
- evidence: 
- where: 
- gap: shared-memory SSBO, 4 constant UBOs, 1x1 stub texture0, render pass into swapchain, one pipeline from the 2 SPIR-V modules. Shaders read geometry from SSBO via vfetch, NOT Vulkan vertex input -- confirmed: hot VS declares only gl_VertexIndex as Input.
- notes: 

