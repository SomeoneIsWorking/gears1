# GearsUE3

GearsUE3 is an in-progress clean-code engine port for the Xbox 360 Gears of War
games. Its target product combines native subsystem overrides with Xenia's
existing x64/A64 Xenon dynarecs for every guest path that remains emulated. It
does not ship an interpreter, generated guest C++, or an offline translation
step.

Gears of War 1 is the active and only conformance target. Gears 2, Gears 3, and
Judgment remain product scope, not compatibility claims. See
`docs/project-state.md` for factual coverage and `docs/gearsue3-engine.md` for
the architecture.

## Migration status

The repository still contains the previous XenonRecomp-generated product and a
substantial body of verified Gears 1 runtime, renderer, audio, input, storage,
and native-override evidence. That implementation is a frozen migration
baseline, not the target product. Do not regenerate, build, or run it during
the migration.

The next executable milestone is intentionally bounded:

1. create `xenonport` around Xenia `Memory`, `Processor`, `ThreadState`, and
   `RawModule`, using Xenia's x64/A64 dynarecs directly;
2. execute the real Gears 1 leaf at `0x8222E868`;
3. resolve and invoke `DbgPrint` as a typed import; and
4. prove disabled, enabled, and scoped-`super` override paths through the Xenia
   dispatcher.

That proves wiring, not game compatibility. The old static path remains in the
tree, untouched, until a fresh image-only build reaches representative
interactive gameplay with nonzero Xenia JIT execution, no gameplay interpreter,
working native/original calls, relevant invalidation coverage, and declared
correctness and frame-time evidence. Only then is the old path deleted in one
milestone; it never remains as a compatibility mode or oracle.

## Preserved evidence

The retired path previously demonstrated exact-revision Gears 1 boot, menus,
Act 1 gameplay, guest threading and memory, SDL-backed input, working XMA audio,
and a bounded Vulkan renderer driven by the title's PM4/Xenos stream. It also
grounded a growing set of title-owned native seams, including resource lifetime
leaf `0x8222E868`. Those observations remain useful as migration targets, but
they are not evidence that the xenonport product exists or passes gameplay.

The compatibility renderer's in-game world is still not fully faithful, saves
do not complete, networking/user services are absent, and no title has passed a
complete exact-revision conformance report. The native RHI is partial and no
representative gameplay run has met the native 8.33 ms / 120 fps goal.

## User-owned game input

No game or UE3 source is included or fetched. The eventual product accepts a
legally owned disc image (or one bounded nested archive), validates its exact
revision, maps the authenticated XEX at runtime, and launches without emitting
or compiling guest code. Explicit CLI input, `GEARS_ISO`/gitignored `.env`, and
one unambiguous ignored `roms/` drop-in remain the intended discovery order.

The current `./run.sh` still implements the retired XenonRecomp pipeline and is
therefore not a supported product route during this migration. It must be
rewired to xenonport before the project again advertises a runnable default.

## Ownership

| Area | Owner |
|---|---|
| Xenon CPU execution, typed imports, image mapping, overrides, original calls | `shared/xenonport` (to be created around the pinned Xenia fork) |
| Cross-framework executable-memory helpers | `shared/jit-common`, only after two integrations prove the same missing contract |
| Exact Gears identity, addresses, policies, navigation, and native bindings | title adapters in this repository |
| Shared Gears engine behavior | cohesive runtime/render/audio/input/storage modules in this repository |
| Authenticated XEX/import facts currently prototyped in `shared/xenon-host` | migrate into `xenonport`; do not retain its generated function-map contract |
| Independent comparison | Xenia oracle/hardware/binary evidence, never the old generated product |

`docs/codemap.md` maps existing and target locations. `docs/re-frontier.md`
preserves the grounded execution/rendering evidence and now names the dynamic
migration chain. Atomic work is in `docs/issues/`.

## Third-party code and licence

`extern/xenia` is a pinned fork of Xenia Canary, BSD 3-Clause. The migration
reuses its Xenon CPU dynarecs as well as its separately integrated Xenos shader
and hardware code. The XMA decoder is built from an LGPL-2.1-or-later FFmpeg
fork. First-party code is intended to be MIT licensed. Gears of War is
copyright Epic Games / Microsoft; this project ships none of it. See `LICENSE`
and `THIRD_PARTY_NOTICES` for the complete notices.
