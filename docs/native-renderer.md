# The native renderer

Status: **seam landed, two passes implemented and bit-exact, two declared.**
Everything below that is not marked DONE is a plan, and this file says which is
which so the next session does not have to guess.

| Pass | Hash | State |
|---|---|---|
| Startup movie YUV→RGB composite | `0xea0007942db096ad` | **DONE** — `runtime/shaders/movie_yuv.frag`, bit-exact against the translated pass (2,764,800 of 2,764,800 channel samples identical on `scratch/frames/boot150.gfr`) |
| Full-screen scene composite (gamma + exposure) | `0x501ac5d8692bf7b6` | **DONE** — `runtime/shaders/scene_gamma.frag`, bit-exact on `scratch/frames/act1.gfr` (2,764,800 of 2,764,800) |
| Height fog | **none — the pass is not in any frame we have** | withdrawn, see below |
| Base pass | many (one per material) | declared, not written. `tools/pass_structure.py --draws BASEPASS` now names the draws |

Run the gate with `tools/verify_native_pass.sh`. It renders the capture through
both paths and refuses to report a match it cannot back: it deletes the screenshot
before each arm (a stale file compares a frame against itself and reports a perfect
match — this already happened once), and it runs a second capture as a negative
control so the comparison is shown reporting a difference in the same run.

## Why

The renderer this project has today reconstructs the frame from the PM4 command
stream: it translates the title's Xenos microcode to SPIR-V, infers state from the
register file, and models EDRAM surfaces, predicated tiling and resolves. It draws
recognisable frames — and it is wrong in ways that are hard to attribute, because
every property of the image is derived rather than known. Two days of this session
went into colour defects that could not be localised, and the reason was always the
same: no step in the pipeline knows what the frame is SUPPOSED to contain.

A native renderer inverts that. The title is Unreal Engine 3, its render passes are
known, and its shading model is published in the engine's own sources. Instead of
asking "what do these registers mean", it asks "which UE3 pass is this, and what
does that pass do".

## The sources this is built against

Not vendored, and never to be committed: UE3 source is licensed and this repository
stays clean (see the rule in CLAUDE.md about copyrighted material). Point
`GEARS_UE3_SRC` at a checkout. The subset that matters here, verified present:

| Path under `$GEARS_UE3_SRC` | What it settles |
|---|---|
| `Development/Src/Engine/Src/BasePassRendering.cpp/.h` | The base pass: what it binds, in what order, and `DrawShared`'s state |
| `Development/Src/Engine/Src/SceneRendering.cpp`, `SceneRendering.h` | The frame's pass order, and the view/scene structures the passes read |
| `Development/Src/Engine/Src/MaterialShared.cpp` | How a material's parameters are laid out — the uniform expressions our UBOs currently mirror by address |
| `Development/Src/Engine/Src/FogRendering.cpp`, `HeightFogComponent.cpp` | Height fog, which is a full-screen pass over the scene colour |
| `Engine/Shaders/BasePassPixelShader.usf`, `BasePassVertexShader.usf`, `BasePassCommon.usf` | The base pass shading itself |
| `Engine/Shaders/MaterialTemplate.usf` | The material interface every generated shader implements |
| `Engine/Shaders/HeightFogCommon.usf`, `HeightFogPixelShader.usf` | The fog maths |
| `Development/Src/Core/Inc/Color.h`, `Src/Color.cpp` | `FLinearColor`, gamma, and the sRGB conversions — the exact place this session's colour questions belong |

## Where it attaches

`docs/d3d-seam.md` has the reconnaissance. The title calls D3D directly (`bl`, not
vtables) through one global device, with a deferred-state model and dirty masks.
`runtime/hle_d3d.cpp` already overrides functions there by defining a strong
`sub_<addr>`, with the recompiled body still reachable as `__imp__sub_<addr>` for a
super-call — so a native pass can be introduced one function at a time, A/B'd
against the recompiled path in the same binary, and reverted by deleting one
definition.

What is known about that seam, measured rather than assumed:

- `SetTexture` (0x82220858) — contract recovered, wrapped, and its per-frame slot
  table cross-checks against the renderer's own texture census: **122 distinct
  bases both sides on a gameplay frame** (catalog #58). DONE.
- `0x82544148` is **not** the per-draw emitter: it fires exactly once per frame
  against 744 draws (catalog #58). The emitter is still unidentified, and eleven
  probed functions have been ruled out by per-frame rate.

## The order to build it in

Step 1 as originally written — "identify the draw emitter" — turned out **not to be
a prerequisite**, and that is the most useful thing this file records. The emitter
is still unidentified (catalog #58, eleven functions ruled out by per-frame rate),
and a native pass shipped anyway. By the time a draw reaches `gpu_draw.cpp` it
already carries the thing that identifies a UE3 pass: the hash of the microcode the
title bound. Substitution keys on that. The emitter hunt is now optional work for
pass-level structure, not a blocker.

1. **~~Identify the draw emitter~~ — not needed.** Key on the pixel-shader hash.
   DONE, `runtime/native_pass.h`.
2. **Write one pass and make it bit-exact.** DONE for the movie composite. The
   method that worked, in order: disassemble the microcode
   (`scratch/shaders/bound_out/<hash>.ucode.txt`), read the *translated* module's
   interface with `spirv-dis` for the descriptor bindings and block layouts,
   write the GLSL, `tools/gen_native_spv.sh`, `tools/verify_native_pass.sh`.
3. **Recover the pass structure**, not the draws. **DONE** —
   `tools/pass_structure.py`, and `GEARS_DRAW_DIAG` now emits a row per resolve so
   the boundaries are visible at all. See "The frame, recovered" below.
4. ~~**Height fog**~~ — **WITHDRAWN, and the withdrawal is the finding.** See below.
5. **The base pass**, which is where the frame's content actually is. This is now
   the next step, and step 3 says exactly which draws it is.

## What the first pass cost, and what it teaches

Reading the microcode was the easy half and I got it right first time. What was
wrong in the first shipped version was the *interface*, and it failed silently:

- **Descriptor bindings are not a choice.** The translator emits, for three texture
  fetches, `set 3: 0/1 = texture0 unsigned/signed, 2/3 = texture1, 4/5 = texture2,
  6,7,8 = the samplers`. Guessing 0,1,2 for textures and 3,4,5 for samplers is not
  a validation error — it samples different images and still draws a recognisable
  picture with wrong colours. Read the bindings off the translated module.
- **Block sizes are not a choice either.** `XeFloatConstants` is sized to the
  constants the shader actually touches (four vec4s here, not 256), and
  `color_exp_bias` is a `vec4` at offset 192, not a float array.
- **The epilogue is part of the pass.** Every translated pixel shader ends with the
  render target's exponent bias and, when system-constant flag bit 14 is set, a
  piecewise-linear gamma encode (the 360's curve, *not* sRGB). A native pass that
  omits it is uniformly the wrong brightness.
- **`dot()` is wrong by one ULP.** The compiler may fuse or reassociate it; the
  sequencer does `(x·a + y·b) + z·c`. The difference crossed an 8-bit rounding
  boundary on four pixels of the test frame. `precise` forbids both transforms.
- **The offline translations in `scratch/shaders/bound_out/` are degenerate.** They
  are translated with no modification key, so they have no interpolator inputs and
  a colour write mask of zero. Trust them for *structure* (bindings, block layouts,
  arithmetic) and never for behaviour.

## What the second pass added

`scene_gamma.frag` was bit-exact on the first attempt, because the first pass had
already paid for the interface knowledge. Two things it added:

- **Constant registers are PACKED, not indexed by register number.** The gamma
  shader names `c0`, `c1` and `c255`; the translator emits a *three*-entry block
  holding exactly those in ascending register order, so `c255` is at index **2**.
  Indexing `c[255]` reads 4 KiB past a 48-byte buffer.
- **Swizzle chains often cancel, and saying so is the reading.** That pass permutes
  channels three times — `saturate(rgb.yxz)`, three `log`s reading `z,x,y`, three
  `exp`s reading `z,y,x`. Composed, the permutations are the identity, so the whole
  predicated block is `pow(saturate(rgb), c0.x)`. Transcribing the swizzles would
  have been correct and unreadable; reducing them is the actual understanding, and
  the A/B is what makes the reduction safe to trust.

## What two bit-exact passes tell us — and what they do NOT

The most important result so far is a **negative**, and it points away from where
this session has been looking:

> Two passes, written independently from the title's microcode and from UE3's
> semantics, agree with the Xenos→SPIR-V translation to the last bit — 2,764,800 of
> 2,764,800 channel samples, twice, on two different captures. **The shader
> translation is not where the graphics defect lives.**

That is worth more than either pass is on its own, because it retires a suspect.
Whatever is wrong with the picture is in state, surfaces, resolves, textures, or
the passes not yet examined — not in the arithmetic the shaders perform.

It also means, plainly: **a bit-exact native pass changes nothing on screen.** It
cannot, by construction. The value of a native pass arrives only when it
*disagrees* with the translation somewhere the translation is wrong. Every match is
a suspect eliminated; only a mismatch is a fix. Anyone reading a "bit-exact" result
here as "the renderer got better" is reading it wrong.

## How each step is verified

Every step lands with the comparison that proves it, on a captured frame replayed
offline (`tools/frame_replay`), because a live run cannot be repeated:

- pixel comparison against the translated path for a pass that should be identical,
- and where it should NOT be identical, the numbers that say why, per channel.

The instruments exist: `GEARS_DRAW_DIAG` (per-draw verdicts), `GEARS_DRAW_RESOLVE_DUMP`
(what each pass produced), `tools/verify_present_path.sh` (that the frame survives
to the screen), and the per-run presented-frame check.

## What this file is not

It is not a claim that a native renderer is close. One pass of four is written, and
it is the smallest one — a full-screen composite with three texture fetches and a
3×3 matrix. The passes that carry the frame's content are the base pass and the
material shaders, and there are hundreds of the latter, one per material. Nothing
here yet addresses the user's actual complaint that the game's rendering is wrong.

What it IS: a seam that is measured, a source tree that is located, a gate that has
been shown to fail as well as pass, and one pass proved bit-exact — so the next
pass is the same procedure again rather than a fresh argument about method.


## The frame, recovered

`tools/pass_structure.py` attributes every row of a `GEARS_DRAW_DIAG` table to a
UE3 render phase. Run on the Act 1 courtyard capture (726 draws + 18 resolves):

```
CLEAR         draw 0
PREPASS       draws 1-167     x167   (1 pixel shader: the pass-through one)
OCCLUSION     draws 168-257   x90
CLEAR         draw 258
BASEPASS      draws 259-432   x174   (44 distinct pixel shaders)   <-- EDRAM tile 1
RESOLVE       draws 433-434          0x400->0xbdf0000 1280x512@0,0
                                     depth 0x0->0xba50000 1280x512@0,0
BASEPASS      draws 435-608   x174   (the same 44 shaders again)   <-- EDRAM tile 2
RESOLVE       draws 609-610          0x400->0xbdf0000 1280x208@0,512
                                     depth 0x0->0xba50000 1280x208@0,512
...           the post chain, then FULLSCREEN 9610bf8038af9aaf and 629226076307234e
```

That is UE3-on-360 exactly as `SceneRendering.cpp` describes it: `RenderPrePass`
(depth only, colour writes off), then `BeginRenderingSceneColor` +
`RenderBasePass`, replayed once per predicated EDRAM tile, each tile resolved to
the scene-colour and scene-depth textures at its own destination offset. The two
base-pass blocks are the SAME 174 draws with the SAME 44 pixel shaders in the same
order — which is what predicated tiling means, and is a self-check the tool
reports rather than an assumption it makes.

**What it will not tell you**, printed with every run: UE3 emits lights
(`RenderDPGLights`), decals (`RenderDecals`), distortion and translucency
(`RenderTranslucency`) with identical register state — colour writes on, blending
on, depth writes off. They are one `BLENDED` band here. Splitting them needs
evidence the register file does not carry: the bound texture set per draw, or the
guest call site (still unidentified, catalog #58).

Verified on four captures — `act1_v2` (157 draws), `courtyard` (726), `bright`
(826), `play_v2` (849) — with **zero rows unattributed** in any of them, and a
`--selftest` that includes a case the classifier must REJECT, because a rule set
that only ever says yes is not a rule set.

## Height fog: withdrawn, with the measurement that withdrew it

The roster declared a height-fog pass and this file put it before the base pass
because "its maths fits on a page". **There is no such pass in this title's
frames.** UE3's `FSceneRenderer::RenderFog` (`FogRendering.cpp:614`) has an
unmistakable signature: a 2-primitive full-screen triangle list, depth test on,
`BO_Add / BF_One / BF_SourceAlpha` blending, and — the distinctive part —
`RHISetColorWriteMask(CW_RED|CW_GREEN|CW_BLUE)`, alpha writes off. It is the only
place in UE3 that sets that mask.

Across all four captures — 2,558 draws — exactly **four** draws have colour mask
RGB, one per capture, and every one of them is the same pixel shader
`0x629226076307234e`, whose microcode is a depth fetch, a reprojection through a
4x4 matrix, a perspective divide, a clamped screen-space velocity and a sampling
LOOP: that is `RenderVelocities`/motion blur, not fog. **Zero draws match
`RenderFog`.**

Why, from the sources: `BasePassCommon.usf` gates vertex fog on
`NEEDS_BASEPASS_FOGGING`, which is
`MATERIALBLENDING_TRANSLUCENT || ADDITIVE || MODULATE || MODULATEANDADD` — so
opaque geometry never carries it, and the full-screen `RenderFog` pass is the only
other producer. If these Act 1 interiors have no fog actor, neither path emits
anything, and that is consistent with everything observed.

**So the entry is withdrawn rather than written.** Writing a fog shader now would
mean picking a hash it does not have, against a draw that does not exist, with
nothing to A/B it against — which is precisely the RE sin this project tracks
(`docs/re-frontier.md`: faking a step's output before its RE is done). If a capture
ever contains a draw with colour mask RGB and `BF_SourceAlpha` blending,
`tools/pass_structure.py` will surface it and the entry comes back.

The general lesson is worth more than the fog: **the order of work was picked from
UE3's source and not from the title's frames, and it was wrong.** Step 3 should
always have come first, because it is what says which passes this game actually
runs.
