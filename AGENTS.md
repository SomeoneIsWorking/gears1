# GearsUE3

GearsUE3 plays the Xbox 360 Gears of War titles as PC games: every guest
instruction runs through Xenia's x64/A64 Xenon dynarec via `shared/x360port`,
and measured native overrides replace hot or host-facing guest functions.
Execution policy: the `dynarec-port` and `dynarec-overrides` skills.

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
- Gears 1 comes first. Finish its compatibility and performance goals before
  title-specific work on Gears 2 or 3.
- `runtime/engine` (the paused native UE3 engine) stays in the tree but is not
  extended.

## Distribution boundary

The repository never contains or fetches UE3 source, game code or assets,
extracted files, decoded shaders, decompiler output, or title-derived caches.
Derived output lives in ignored `scratch/titles/<fingerprint>/` and must be
regenerable.

## Commands

- Build: `uv run --locked cmake --build build/product --target gears1`. Tests
  are CTest targets in `build/product`.
- Agent runs: `tools/run_offscreen.py` with `--control-port`, and
  `tools/product_control.py` to drive input and read state. `tools/combat_route.py`
  plays Act 1 closed-loop over that channel. Never print the image path or
  process arguments, and never print `.env` values.
