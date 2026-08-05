# The native renderer

Status: **design + seam, no passes implemented yet.** Everything below that is not
marked DONE is a plan, and this file says which is which so the next session does
not have to guess.

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

1. **Identify the draw emitter.** Nothing native can attach to the draw stream until
   the function that emits `DRAW_INDX` is known. Address-guessing is exhausted;
   the method that replaces it is the write-watch already in `hle_d3d.cpp`
   (`WatchArm`/`WatchProtect` mprotects a guest page and reads the faulting
   context) pointed at the ring pages, which names the writer from its LR.
2. **Recover the pass structure**, not the draws: which draw ranges belong to which
   UE3 pass. `SceneRendering.cpp` gives the order; the per-draw diag table already
   groups draws by surface and shader pair, so the two can be aligned.
3. **Implement one pass natively** — height fog first. It is a single full-screen
   pass with maths that fits on a page (`HeightFogCommon.usf`), it reads scene
   colour and depth which the renderer already produces, and it can be A/B'd
   against the translated version pixel for pixel on a captured frame.
4. **Then the base pass**, which is where the frame's content actually is.

## How each step is verified

Every step lands with the comparison that proves it, on a captured frame replayed
offline (`tools/frame_replay`), because a live run cannot be repeated:

- pixel comparison against the translated path for a pass that should be identical,
- and where it should NOT be identical, the numbers that say why, per channel.

The instruments exist: `GEARS_DRAW_DIAG` (per-draw verdicts), `GEARS_DRAW_RESOLVE_DUMP`
(what each pass produced), `tools/verify_present_path.sh` (that the frame survives
to the screen), and the per-run presented-frame check.

## What this file is not

It is not a claim that a native renderer is close. Step 1 is unsolved. The value of
writing it down now is that the next session starts from a seam that is measured,
a source tree that is located, and an order of work whose first step is a
measurement rather than a guess.
