# GearsUE3 guidance

The global rules in `../../AGENTS.md` apply. Read `docs/project-state.md` and
`docs/codemap.md` before changing a subsystem, and update the authority whose
answer changes in the same commit.

## Product

GearsUE3 plays the Xbox 360 Gears of War titles as PC games: every guest
instruction runs through Xenia's x64/A64 Xenon dynarec via `shared/x360port`,
and measured native overrides replace hot or host-facing guest functions.
`x360port` may interpret a block only after compilation fails, an instruction
is unsupported, or generated host code is unsafe, with the reason recorded.
Interpreter mode is diagnostic and never gameplay or performance evidence.

```text
Gears title/revision adapters + GearsUE3
        -> shared/x360ue3 -> shared/x360port -> Xenia dynarec
```

- `x360port` owns the Xenia embedding (`SystemSession`, memory, processor,
  threads, typed imports, overrides, scoped original calls) and Xenia's
  process-global memory, MMIO, and clock assumptions. Do not write another PPC
  interpreter, decoder, or emitter, and do not put Xenia behind `jit-common`.
- `x360ue3` holds reusable UE3-on-360 contracts only; no Gears address, hash,
  pass roster, navigation, save policy, gameplay, or composition.
- This repo owns Gears policy. A title/revision adapter
  (`runtime/titles/<title>/`) owns exact image identity, override bindings,
  probes, pass hashes, save namespace, and scripted navigation; unknown
  revisions refuse. `runtime/product/` composes the shipping executable.
- Normal calls honor the override table; `super` suppresses only the current
  override and re-enters the original guest address. Mutating an override
  invalidates translated paths that captured the old decision.
- Gears 1 comes first. Finish its compatibility and performance goals before
  title-specific work on Gears 2 or 3.
- `runtime/engine` (the paused native UE3 engine) stays in the tree but is not
  extended.
- The retired generated-PPC product (translator, generated modules, function
  maps, precomputed dispatch) must not return.

## Clean distribution boundary

The public repository holds independently authored source, compatible
open-source dependencies with their notices, and factual interoperability
metadata. It never contains or fetches UE3 source, game code or assets,
extracted files, decoded shaders, decompiler output, or title-derived caches.
The user's own disc image is the only copyrighted input; derived output lives in
ignored `scratch/titles/<fingerprint>/` and must be regenerable. `shared/ue3` is
developer reference only: nothing from it is compiled, linked, packaged, or
copied here.

## Building and running

- Build with Clang: `uv run --locked cmake --build build/product --target gears1`.
  Run the focused test for what you changed, the structure check with its
  self-test, and the combined gate once edits are frozen.
- Python tools run as `uv run --locked python <tool>` from the root
  `pyproject.toml`/`uv.lock`; no ambient interpreter or second environment.
- Agent runs are headless: `tools/run_offscreen.py` with `--control-port` and
  `tools/product_control.py` to drive input and read state. Never run
  `./run.sh`, never print the image path or process arguments, and never print
  `.env` values.
