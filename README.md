# GearsUE3

GearsUE3 is an in-progress clean-code engine port for the Xbox 360 Gears of War
games. Its target product combines native subsystem overrides with Xenia's
existing x64/A64 Xenon dynarecs for every guest path that remains emulated. A
bounded Xenia interpreter fallback is allowed only after a reason-labelled
dynarec failure; it is counted and cannot satisfy gameplay or performance
evidence. The product has no generated guest C++ or offline translation step.

Gears of War 1 is the active and only conformance target. Gears 2, Gears 3, and
Judgment remain product scope, not compatibility claims. See
`docs/project-state.md` for factual coverage and `docs/gearsue3-engine.md` for
the architecture.

## Migration status

The previous generated-code product and its build, dispatch, configuration, and
generation surfaces have been removed. The preserved runtime source is native
subsystem work awaiting adaptation to x360port's canonical CPU/memory/service
interfaces; it is not a buildable compatibility CPU path.

The next executable milestone is intentionally bounded:

1. create `x360port` around Xenia `Memory`, `Processor`, `ThreadState`, and
   `RawModule`, using Xenia's x64/A64 dynarecs directly;
2. execute the real Gears 1 leaf at `0x8222E868`;
3. resolve and invoke `DbgPrint` as a typed import; and
4. prove disabled, enabled, and scoped-`super` override paths through the Xenia
   dispatcher.

That proves wiring, not game compatibility. It runs only after the old CPU path
is absent. A fresh image-only build must still reach representative interactive
gameplay with Xenia's dynarec selected by default, bounded and counted fallback,
working native/original calls, relevant invalidation coverage, and declared
correctness and frame-time evidence. Fallback execution and explicit diagnostic
interpreter mode do not qualify that gameplay result.

## Preserved evidence

The retired path previously demonstrated exact-revision Gears 1 boot, menus,
Act 1 gameplay, guest threading and memory, SDL-backed input, working XMA audio,
and a bounded Vulkan renderer driven by the title's PM4/Xenos stream. It also
grounded a growing set of title-owned native seams, including resource lifetime
leaf `0x8222E868`. Those observations remain useful as migration targets, but
they are not evidence that the x360port product exists or passes gameplay.

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

The current `./run.sh` refuses explicitly because `shared/x360port` has not yet
provided the Xenia executor boundary. It cannot generate, build, or select the
removed product.

## Ownership

| Area | Owner |
|---|---|
| Xenon CPU execution, typed imports, image mapping, overrides, original calls | `shared/x360port`; its validator slice exists, while the pinned-Xenia executor remains missing |
| Reusable UE3-on-Xbox-360 ABI, RHI semantics, and engine lifetimes | `shared/x360ue3`, consuming only public `x360port` interfaces |
| Cross-framework executable-memory helpers | `shared/jit-common`, only after two integrations prove the same missing contract |
| Exact Gears identity, addresses, policies, navigation, and native bindings | title adapters in this repository |
| Shared Gears-family engine behavior | `GearsUE3`: cohesive runtime/render/audio/input/storage modules in this repository |
| Authenticated image/import validation | `shared/x360port/x360port_validation`; it must next be exercised against Xenia `RawModule` |
| Independent comparison | Xenia oracle/hardware/binary evidence, never the old generated product |

The local `shared/ue3` checkout is developer reference material, not one of
these runtime layers. No product may compile, link, package, copy, or require it.

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
