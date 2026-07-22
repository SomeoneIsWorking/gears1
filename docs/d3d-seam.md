# The D3D seam — reconnaissance for the HLE graphics backend

Status: reconnaissance only, no backend code. Everything below is either
verified against raw disassembly / live runs (marked **V**) or explicitly
flagged as unverified inference (**?**). Method and evidence trails are in
catalog entries #12–#15.

## 1. Shape of the API surface

The 360's D3D is statically linked into the title. The surface is:

- **One global device object.** Reached as `**(0x82000868)`; observed instance
  at guest 0x4015B080. All API functions take it in r3 and most return it
  (call-chaining: `r3 = Set*(r3, value)` — see 0x8254ED40, which chains nine
  setters). **V**
- **Direct `bl` calls, not vtables.** UE3's RHI layer calls D3D functions
  directly; there is no COM-style vtable on the device for the hot paths.
  Vtable dispatch exists one layer up (UE3 render-command `Execute` virtuals,
  consumed by the render thread loop 0x82444EF0) and in small callback
  objects, but the D3D API itself is a flat set of guest functions. **V**
- **Deferred-state model.** Setters do not touch the GPU: they write shadowed
  state into the device struct and OR bits into 64-bit dirty masks at
  device+0x10 / device+0x18 (seen throughout 0x8254ED40 and the setter
  bodies). The draw call flushes dirty state as TYPE0 register writes into the
  command buffer. The flush-and-draw emitter is **sub_82544148** (the largest
  function in the render path — ~21k lines of recompiled code). **V**
- Address ranges are NOT a reliable layer boundary: e.g. 0x8221CBA8 sits in
  the "D3D range" but is the UE3 render-command ring allocator (writes the
  ring control block at 0x82C0CB24). Classify per function, not per range. **V**

### Known internals (below the API — do not hook these for HLE)

| Address | Role (all **V**) |
|---|---|
| 0x82221380-ish body w/ 0x82221424 | ring kick: appends IB packet, stores CP_RB_WPTR (MMIO 0x7FC80714) |
| 0x82221640 | per-submission retirement template (EVENT_WRITE_SHD fences, tickets at pool+0x2A1C step 2) |
| 0x82221A68 / 0x8222F460 / 0x8222FD78 | ticket lock / adaptive waiter / "GPU is hung" escalation |
| 0x82221C60 | graphics ISR (source 1 = command-complete, source 0 = vblank) |
| 0x82221EC0 / 0x8223A840 | device (re)init, ME_INIT, scratch writeback setup |
| 0x8223E3E0 | Present implementation — fills the reserved swap block via VdSwap (two `bl` sites 0x8223E43C/0x8223E488) |
| 0x8223E860 | present/retire pump (worker thread + UE3 both call it) |
| 0x8223B7D0 / 0x8223B5E0 | D3D worker thread and its CPU command-list interpreter |
| 0x825B48B8 | render-command executor with the 16-case jump table at 0x825B48F8 |

## 2. Entry points of one presented frame

Measured with gdb ignore-count breakpoints over ~30 s of the movie phase
(the only phase that presents today; counts normalised per VdSwap=414).
The post-load state currently presents nothing and calls none of these —
so this set IS the "first presented frame" target. **V**

| Function | hits/swap | Identification (confidence) |
|---|---|---|
| 0x82220858 | 6.2 | SetTexture: copies fetch-constant words from a guest texture object (+0x1C..+0x30) into the device's fetch shadow (slot index r4, shadow at dev + (slot+0xBFC)*4) (high) |
| 0x8222CFF8 | 1.02 | per-frame state set, writes dirty bit 0x8000000000000 (medium — viewport/scissor-shaped) |
| 0x82221980 | 1.01 | ring segment flush / space wait (internal, high) |
| 0x82222460 | 1.0 | dirty-mask OR helper + cache touch (high) |
| 0x8221CBA8 | ~1 per render command | UE3 render-command ring alloc — NOT D3D (high) |
| 0x82228998 + 8 siblings (0x82229B28, 0x82228A28, AB8, B48, D28, BD8, C48, CB8) | 0.4 each | sampler-state setter family, applied as a 9-field block by 0x8254ED40 (high) |
| 0x82222350 | 0.38 | state setter (unidentified) |
| 0x822193D8 / 0x822193B0 | 0.28 / — | free / alloc utility pair (575/420 static callers) (medium) |
| 0x8222ABF8 / 0x8222AB30 | 0.15 | paired begin/end-shaped calls (per movie frame) (low) |
| 0x8222E8E0 | 0.12 | setter with 99 static callers incl. thunk arrays (unidentified) |
| 0x8222B068 / 0x8222B398 | 0.08 | paired (set/unset-shaped) (low) |
| 0x8221D9B8 | 0.07 | async/block-wait helper (appears in movie + resource paths) (low) |
| 0x82544148 | ~0 in movie phase | THE draw flush (state → TYPE0 packets + draw); the movie path instead draws via 0x8221D3A8 | 
| present chain | 1.0 | UE3 wrapper 0x824A5170 → 0x8223E860 → 0x8223E3E0 → VdSwap (high) **V** |

Full static surface: 201 distinct functions in 0x82218000–0x82241000 are
called from outside that range; 51 of them from the RHI/draw zone
(0x82540000–0x82560000). The list with caller counts is reproducible with
the `bl`-scan used here (see catalog #15). An HLE seam does not need all
201 — the movie frame uses ≈20.

## 3. Shaders at the seam

- Shader objects wrap a container whose magic is **0x102A11tt** (big-endian),
  where `tt` is the shader type: **0 = pixel, 1 = vertex**. This was previously
  recorded as a single constant 0x102A1100; that was the pixel-shader case
  only. Verified over 1290 containers: every `tt`=0 container carries the
  D3D9 token 0xFFFF0300 (ps_3_0) and every `tt`=1 container carries
  0xFFFE0300 (vs_3_0), 0 exceptions. **V**
- Two containers are embedded in the image itself (0x82039878, 0x82039A40 —
  D3D's built-ins). They are **not two shaders**: both are the same pixel
  shader with the same microcode, differing only in the 16-float constant
  block (two YUV→RGB matrices). The rest arrive from cooked UE3 packages. **V**
- **Container layout (now mapped, verified — see `tools/shader_extract.py`)**:

  | Offset | Meaning |
  |---|---|
  | +0x00 | magic `0x102A11tt` |
  | +0x04 | `headerSize` — also the offset of the data blob |
  | +0x08 | offset, role not identified |
  | +0x0C | always 0 |
  | +0x10 | offset of a u32 CTAB byte-size, with the D3D9 constant table at +4 after it (its `Version` word is the 0xFFFF0300 / 0xFFFE0300 token) |
  | +0x14 | offset of a 0x28-byte section, or 0 if absent |
  | +0x18 | offset of the shader-info section, last thing in the header |

  Shader-info section: word 0 = constant-block byte size, word 1 = **microcode
  byte size** (always a multiple of 12 — the Xenos instruction slot size);
  words 2.. are register-shaped and not decoded. The blob after the header is
  `[constants][microcode]`, so the microcode starts at
  `headerSize + info[0]` and is `info[1]` bytes long.

  Evidence: parsed 5443 magic hits across 768 disc files; 1290 satisfied every
  structural check with **zero** rejections among them, and the two built-ins
  decode to exactly the movie player's YUV→RGB converter (three `tfetch2D` of
  Y/U/V samplers, then the colour matrix). The **?** on this line is cleared.
- Validation site: 0x82227600 region compares against the magic at create
  time (device+0x4D34/0x4D3C/0x4D40 involved — a shader/constant cache).
  UE3-side loaders referencing the magic: 0x82617464, 0x8261D58C,
  0x826223E0, 0x82694CD0, 0x8274679C. **V**
- Consequence: **shader translation input = Xenos microcode**, not D3D9
  bytecode; the 0xFFFF0300 token is metadata only. This is the same
  translation job Xenia solved (ucode → host shaders) and it is unavoidable.

### Translation status: proven on this title's own data

`xenia_gpu/` compiles Xenia's microcode front end and SPIR-V back end into our
tree; `tools/xenos_translate/` drives it offline. Measured result:

- **425 distinct microcode payloads** extracted from the cooked packages
  (322 pixel, 103 vertex) plus the built-in.
- **425 of 425 translate**, and **425 of 425 pass `spirv-val --target-env
  vulkan1.3`** (SPIRV-Tools v2026.1). No shader is special-cased.
- The **capability shim was not needed**. `SpirvShaderTranslator::Features`
  already has a device-free constructor; we pass `Features(all=true)`. The
  only host code we had to supply is `xenia_gpu/xenia_host_shim.cpp`
  (`ShowSimpleMessageBox` / `LaunchWebBrowser` / `LaunchFileExplorer`), which
  exists purely to avoid linking SDL2 for a message box the translator can
  never raise.
- **Xenos vertex stride does not come from the shader in this title.** All 103
  vertex shaders leave the `vfetch_full` stride field zero and trip Xenia's
  `assert_not_zero(fetch_instr.attributes.stride)`
  (`shader_translator.cc:421`) — confirmed by backtrace on all 103, and by
  decoding the raw instruction words (`opcode=0 stride=0 offset=0`). The
  microcode either side of the assert disassembles cleanly, so this is not a
  parse error: **the HLE layer must supply the vertex stride from the vertex
  fetch constant at bind time.** Xenia's asserts are therefore off by default
  in `xenia_gpu/` (`-DGEARS_XENIA_ASSERTS=ON` to restore them).

What this does **not** establish: the SPIR-V is well-formed, not proven
correct — nothing has been executed on a GPU and no output has been compared
against the console.

### Which shaders the title actually binds (measured)

The offline corpus above is a corpus of **templates**, and it is not what runs.
Both questions left open — where shaders are bound, and which ones — are now
answered by measurement. Full evidence in catalog entries #21 and #22.

**Where they are bound.** Two D3D API setters, identified by probing all 51
D3D-range functions reachable from the UE3 RHI zone and keeping the ones
measurably handed a shader object (`GEARS_SHADER_ARGSCAN=1`):

| Address | Role | Device field | Dirty bit(s) at device+0x10 |
|---|---|---|---|
| **0x82222808** | `SetPixelShader` (r3 device, r4 shader) | device+0x3080 | 1<<52, 1<<49 |
| **0x82222B98** | `SetVertexShader` (r3 device, r4 shader) | device+0x3084 | 1<<51 |

Called exactly the same number of times as each other (410374 each over four
minutes); 0x82222808 is only ever handed type-0 (pixel) objects and 0x82222B98
only type-1 (vertex) ones. The flush emitter at 0x82234xxx reads both fields and
emits the sequencer load. **The shader object is not the bare container**: the
0x102A11tt word sits at +0x28 inside a pixel-shader object and +0x368 inside a
vertex-shader one. **V**

**The authoritative bind point is lower.** The GPU is not given the container's
microcode; it is given a patched copy, via the PM4 sequencer loads
`IM_LOAD` (opcode 0x27, microcode by physical address) and `IM_LOAD_IMMEDIATE`
(opcode 0x2B, microcode inline). Both occur in this title's stream (~181 and ~33
packets per frame). `runtime/vd_null_gpu.cpp` captures them under
`GEARS_SHADER_CAPTURE=1`. **V**

**The patch is the vertex fetch.** Every `vfetch_full` in captured vertex
microcode carries a non-zero stride (2..22 observed); `Stride=0` never occurs,
where the corpus has it everywhere. For 12 of the 18 bound vertex shaders there
is a corpus container of the same length whose disassembly differs **only** in
`vfetch` lines — the ALU body is byte-identical. So the guest D3D merges the
vertex fetch constant into the instruction at bind time, exactly the mechanism
predicted in catalog #20, and a shader must be translated **after** that patch.
**V**

**What is bound, over a 10-minute run** (2.0M `IM_LOAD` + 0.36M
`IM_LOAD_IMMEDIATE`, 0 rejected, 0 truncated):

- **38 distinct microcode payloads**: 18 vertex, 20 pixel. 5 of them during the
  movie phase; the set reaches 38 at the first post-load frame and never grows
  again, over ~18000 frames.
- **38 of 38 translate to SPIR-V and pass `spirv-val --target-env vulkan1.3`**
  (`xenos_translate --raw`).
- Against the 425-container offline corpus: **9 byte-identical** (all pixel),
  **12 more are corpus shaders with the vertex fetch patched**, and **17 have no
  established corpus counterpart**. So the compressed-chunk gap is not the main
  story — fetch patching is.
- The hot set is tiny: one vertex/pixel pair (`vs_5363d074`/`ps_501ac5d8`)
  accounts for **61%** of all binds, the top six for **81%**, and the
  top ten for **89%**. Early rendering work can target a handful.

Caveat on the counts: the title replays its command buffer once per EDRAM tile
(predicated tiling), so a bind is counted once per tile pass. That inflates the
absolute numbers equally across shaders; the shares are still meaningful, the
absolute "binds per frame" is not.

Still open: the corpus is what is stored **uncompressed** in the packages, so
whether the 17 unmatched payloads come from compressed package chunks or are
simply patched beyond recognition is not established. Nothing has been executed
on a GPU. And the run only ever reached the phases the port reaches today — a
title that got further would bind more.

## 4. Resources

- **Textures**: guest objects whose header (offsets +0x1C..+0x30 observed in
  SetTexture) already contains the **6-dword Xenos fetch constant** —
  physical base address, tiled format, endian swizzle baked in at creation
  or cook time. Draw flush emits them as TYPE0 writes to registers 0x4800+
  (observed in live streams: `TYPE0 reg 4800 cnt 6`). **V**
- **Shader constants**: ALU constant file via TYPE0 writes at 0x4000+
  (observed `reg 4000 cnt 31`). Set by value at the API, so HLE-friendly. **V**
- **Vertex/index data**: physical guest memory referenced by vertex fetch
  constants (not yet individually traced — **?**).
- **Render targets / EDRAM**: the draw streams program RB/PA register ranges
  (0x2000/0x2100/0x2200/0x2300 series observed) that describe EDRAM surface
  layout; completed frames are moved out by resolve sequences. The title
  assumes the EDRAM model exists (10 MB, tile offsets). Not mapped in
  detail (**?**), but the movie path's use is simple (one color surface,
  resolve to the front buffer at 0xA030C000 — observed as VdSwap's front
  buffer argument).
- Verdict for question 4: the title hands the API **guest objects containing
  precomputed hardware descriptors**, not creation-time descriptions. For
  HLE this means either (a) hook creation APIs too and keep a guest→host
  resource map, or (b) parse fetch constants at bind time (Xenia's LLE
  approach). (a) is the HLE-consistent choice but creation entry points are
  not yet identified (candidates 0x82227120/0x82227000 called from UE3
  texture code at 0x8264Dxxx — unverified, **?**).

## 5. Honest size assessment

Minimal path to one presented frame (the movie/loading screen — a textured
quad + UI), in dependency order:

1. **Hook set**: ~20 functions (section 2) + the present chain + texture/
   shader/buffer creation entries (still to be identified). The mechanism
   exists (function-table override keyed on guest address); a registration
   helper is trivial.
2. **Resource map**: guest texture object → host texture, including
   360 tiling and endian detiling for the formats the movie path uses
   (start with linear/DXT + 8888). Bounded, well-documented problem.
3. **Shader translation**: Xenos microcode → host shaders. This is the hard,
   unavoidable core — weeks, not days, even cribbing structure from Xenia's
   translator, and it must exist for ANY real frame: the movie quad already
   uses real shaders. There is no "skip shaders" first frame unless we
   special-case the movie blit (recognize the built-in shaders at
   0x82039878/0x82039A40 and hand-write their host equivalents — a
   legitimate first milestone, NOT a general solution).
4. **EDRAM/resolve semantics**: for the movie frame, one render target and
   one resolve; can be modelled as "render to host texture, present it".
   Full games use multi-surface EDRAM tricks (depth reuse, tile offsets,
   predicated tiling) — that is the long tail, and pretending otherwise
   would be optimistic.
5. **What must be accepted as wrong initially**: no XMA audio (unchanged);
   post-load progression is blocked on something not yet diagnosed (the
   title stops presenting and stops calling the frame API after loading —
   entry #15); HLE does not fix that by itself.

Blunt totals: *first movie frame on screen* = hook layer + resource map +
two hand-written shaders ≈ small weeks. *General scene rendering* = full
ucode translator + EDRAM model + the remaining ~180 API functions as they
appear ≈ the single largest remaining subsystem of the port, comparable to
everything done so far combined. The LLE command processor we have keeps
the title alive meanwhile and its packet knowledge (fetch constants, dirty
state, register footprint) transfers directly into the HLE resource/state
model.

## Not established (explicit)

- Creation-API entry points for textures/buffers/render targets.
- The roles of container header words +0x08 and +0x14, and of shader-info
  words 2.. — enough of the container is mapped to find the microcode and its
  type, but those fields are still unread.
- Whether the 17 bound microcode payloads with no corpus counterpart come from
  compressed package chunks or are patched beyond recognition (section 3).
- Whether pixel shaders are patched at bind time the way vertex shaders are.
- Vertex declaration / stream binding entry points.
- What the post-load state waits on (it calls none of the frame APIs;
  separate investigation, catalog #15).
- Whether any title path pokes the command buffer without going through the
  API (the movie player writes its own draw via 0x8221D3A8 inside D3D — an
  HLE layer must hook at or above that function, or keep the LLE CP for
  D3D-internal paths during transition).
