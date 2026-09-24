# Reverse-engineering frontier

This is the ordered evidence chain toward a faithful Gears 1 product. It records
what must be proven next; capability status belongs in `docs/project-state.md`.

## Gears 1 execution frontier

### RE-01 — Title identity
- status: re-verified
- deps:
- evidence: config/titles/gears1.toml; headless ignored-XEX discriminator

The Gears 1 profile authenticates both the
   original container and normalized executable image. Unknown revisions refuse.

### RE-02 — Reusable host semantics
- status: re-partial
- deps: RE-01
- evidence: focused native renderer, kernel, audio, frame, and wait tests
- gap: Native owners are not composed with the authenticated guest executor.

Native renderer,
   kernel, audio, frame, and wait components have focused tests and retain only
   executable-address/ABI facts. They currently have no guest executor.

### RE-03 — Typed Xenia execution boundary
- status: re-partial
- deps: RE-01
- evidence: pinned x360port synthetic runtime gate; headless real-image leaf discriminator
- gap: Complete title services and complete interpreter fallback coverage are missing.

The exact shared
   `x360port` and maintained Xenia revisions now own memory/module/context
   lifetimes, authenticated synthetic image mapping, JIT calls, typed imports,
   and tested image-scoped native override/original-call and device-memory
   callback seams. Explicit and automatically observed executable-write
   invalidation are now proven for the authenticated virtual code range. The
   shared runtime now also bounds translated basic-block entries across nested
    guest calls and reports typed exhaustion and mid-call invalidation; the bounded
    fallback remains a partial shared contract. The shared runtime test separately proves a
   translated guest caller reaches a second guest callee through Xenia and
   reuses both translations.

### RE-04 — Synthetic Gears-addressed discriminator
- status: re-partial
- deps: RE-03
- evidence: test_gears1_dynarec_boundary
- gap: Synthetic code proves composition only, not the real leaf or gameplay.

An asset-free image
   whose code and entry point use `0x8222E868` translates through Xenia, crosses
   a typed `DbgPrint` import into native code, and returns with nonzero translation/emission counts.
   This proves the composition seam, not the real leaf body.

### RE-05 — First real guest discriminator
- status: re-partial
- deps: RE-04
- evidence: headless test_gears1_real_leaf against the profile-matching ignored XEX
- gap: Most title-specific services and the complete launch path remain absent.

The shared
   `x360port::MapPeImage` owner and `Gears1GuestImage` adapter now authenticate
   the profile-matching normalized image and map its PE sections into the flat
   guest image contract; synthetic and real ignored-input adapter runs pass. The
   shared checked-XEX2 inspector also matches the real container, normalized image,
   236 logical imports, and eight helper-pattern hits. The maintained headless
   discriminator allocates and initializes a caller-owned guest object and
   variable-import storage through `x360port`, invokes a retained real-image
   function-import thunk through the authenticated manifest, then executes the
   real `0x8222E868` body, proving its result, native override/scoped original
   path, and both reported-write and automatic virtual-write invalidation. The
   authenticated PE's `0x8222E8C0` call returns to `0x8222E868` when a resource
   first gains a reference with type nibble 4, flag `0x40000000`, and a linked
   resource at offset 24. A headless real-image fixture proves that call enters
   the native override and that removal restores the original nested call.
   The inspector resolves the two XEX library-table entries by index, retaining
   `xam.xex` and `xboxkrnl.exe` as distinct bindings; service semantics are still
   mostly unimplemented. The title-owned `XGetAVPack` ordinal 971 and shared
   `XamInputGetCapabilities`/`XamInputGetState` ordinals 400/401 bindings are
   proven through their real thunks.
   `Gears1Runtime` now composes the authenticated adapter with a persistent
   `x360port::RuntimeContext`, bounded variable-import storage, and the known
   `XGetAVPack` service; an asset-free entry test executes that route through
   Xenia and returns the configured AV pack.
   The maintained real-image discriminator now consumes this same owner for
   checked-XEX initialization, all 236 import bindings, guest allocations,
   input service calls, and native override/original execution; its readback
   assertions use the bounded `ReadGuestMemory` operation rather than a second
   import-binding implementation.
   The product still needs title-specific import/device services and a complete
   launch path before this is title conformance.

### RE-06 — Fallback discriminator
- status: re-partial
- deps: RE-03, RE-05
- gap: The shipping fallback is bounded and reason-labelled, but only covers a small PPC subset.

The shared runtime test forces an invalid opcode through the bounded fallback and proves the
typed unsupported refusal without host-code publication. It then executes decoded-but-uncompiled
fixtures through Xenia's bounded interpreter, verifying `lswi`, integer immediates, big-endian
loads/stores, comparisons, and conditional branches, plus entry, instruction, unsupported,
memory, and budget counters. Explicit interpreter mode remains diagnostic-only. Fallback results
do not satisfy gameplay or performance. The remaining gap is complete PPC semantics and safe
guest control flow/import/device behavior on real title code.

### RE-07 — Boot and subsystem restoration
- status: todo
- deps: RE-02, RE-05, RE-06
- gap: Guest execution is not composed with complete title services.

Reconnect imports, scheduling,
   GPU, audio, input, storage, and frame identity one owned boundary at a time.

### RE-08 — Representative interactive gameplay
- status: todo
- deps: RE-07
- gap: No interactive Gears 1 gameplay through the new runtime is qualified.

Compare deterministic CPU
   state, relevant memory, devices, audio, rendering, and frames with the oracle.

### RE-09 — Native renderer completion
- status: todo
- deps: RE-02, RE-08
- gap: Live Xenia-fed native rendering is not yet verified.

Replace guest command construction
   only after the semantic plan and native backend agree on same-run resources,
   lifetime, synchronization, shader state, resolves, presentation, and pixels.

### RE-10 — Platform and performance qualification
- status: todo
- deps: RE-08, RE-09
- gap: A64 host qualification and representative 8.33 ms native rendering remain absent.

Qualify x86-64, Apple
   Silicon macOS A64, and Android arm64-v8a independently; then measure the native
   8.33 ms renderer target on named hardware.

## Retained exact facts

- Resource AddRef/Release entry points are `0x8222E868` and `0x8222E8E0`.
- The Gears 1 audio-mix operation begins at `0x825F2D40`.
- Guest addresses index the image as the XEX loader leaves it: the decompressed
  basefile copied flat, each section at its raw offset. Gears 1's PE section
  headers claim VirtualAddresses above those offsets (`.text` claims 0x170000
  but sits at 0x16B200), so a copy re-laid by VirtualAddress decodes a
  different, still-plausible function at every address from `.text` on.
  Reverse-engineering reads the loaded image via `tools/guest_image.py`, which
  refuses the re-laid copy; `x360-xex-inspect --image-out` writes it.
- Normal draw entry points are `0x8222CFF8`, `0x8222D4F8`, `0x8222DA48`, and
  `0x8222DE50`; shader setters are `0x82222808` and `0x82222B98`.
- Shader-state flush `0x822346A8` emits ordered Xenos `IM_LOAD` packets and may
  roll command storage through `0x82221980`.
- The game-authored command-list interpreter at `0x8223B2AC` consumes UE3/Xenos
  command records. This names game behavior, not a CPU execution mode.
- The engine's own map change: `PrepareMapChange` `0x82426D98`,
  `IsReadyForMapChange` `0x824272C0`, `ProcessAsyncLoading` `0x8242AFF8`, and
  `FName::FName` `0x82364678`, called in that order by `sub_821B4620` at
  `0x821B4F0C..0x821B4F48`. The commit step is not identified.

These facts must be re-observed through the authenticated Xenia context before
they authorize dispatch or a shared `x360ue3` contract.

### Local player

Read live through `GET /api/memory` on sp_prison_p; `runtime/titles/gears1/player_probe.*`
follows this chain for `GET /api/player`:

- `0x82BED138` holds the `UGameEngine` pointer. A UObject begins with its
  vtable, Outer at `+0x28`, name at `+0x2C`, Class at `+0x34`, and archetype at
  `+0x38`.
- Engine `+0x29C` is the local-player array (data, then count at `+0x2A0`);
  a local player's `+0x40` is its player controller.
- Controller `+0x1A0` is the pawn, null while the player is dead; pawn `+0x1AC`
  points back at the controller. Controller `+0x294` is the camera actor.
- An actor's `+0x8C` is the level's WorldInfo; WorldInfo `+0x288` and `+0x28C`
  are floats that each advanced 0.999-1.000 game seconds per wall second over
  5 s and 20 s of play at 120 presents per second, the game and real clocks.
- Actor location is three floats at `+0xCC`; rotation is pitch, yaw, roll at
  `+0xD8`, 65536 units per turn and accumulating past a turn. Left-stick
  movement follows the controller's yaw; the world is left-handed, so full
  right moves a quarter turn toward +y of +x. Holding Y turns the camera actor,
  not the controller, toward the point of interest.
- Pawn `+0x35C` is the held weapon; weapon `+0x490` rises by one for each
  round fired and falls when RB reloads (a Lancer burst adds 12; one pistol
  shot moved it from 0 to 1 and RB reset it). Weapon `+0x450` is not the
  magazine size: a Hammerburst read 26 there while firing 78 rounds without a
  reload, and the HUD's 048 matched `+0x490` = 30. `+0x498` fell from 36 to 35
  on a pistol reload but its meaning for other weapons is unknown. D-pad right
  held the rifle and down the pistol. Pawn `+0x358` is the inventory manager.
  Found by differencing the weapon across bursts; the HUD's ammo totals appear
  elsewhere in memory but never changed with firing, so their owner is still
  unknown.
- WorldInfo `+0x328` heads the pawn list, linked through pawn `+0x1B0`;
  WorldInfo `+0x324` heads the controller list, linked through controller
  `+0x1AC`. At the yard's first firefight the pawn list held Marcus, Dom, and
  four Locust drones: two in the yard near (-1470, 4830) and (-1630, 4550) at
  z 203-220 and two on the balcony near (-3195, 5550-5850) at z 845. Pawn
  vtable `0x820ABC18` was both COG pawns and `0x820AB558` every drone.
- Pawn `+0x288` is current health, a signed int: 301 for Marcus and 250 for a
  drone when full, falling with hits, regenerating in cover, and zero or below
  once dead (-750 after some kills). Found by differencing Marcus while he was
  shot to death. Pawn `+0x48C` is the same 301/250 but did not fall, so it is a
  maximum. The first byte of pawn `+0x3B4` is the team: 0 for COG, 1 for every
  drone.
  A downed squad mate (Dom at `WarCheckpoint_3`) reads health 0 while alive and
  revivable. The REVIVE tutorial prompt shown then ignores the stick and the
  trigger until A dismisses it, while both WorldInfo clocks run on: over 2.6 s
  with it shown `+0x288` and `+0x28C` each advanced 2.6 s, the stick moved
  Marcus 0 units and RT fired no round; after A, RT fired.
- The camera actor's location is the view's origin, not the pawn's: about 40
  units to the pawn's right, 70 behind and 66 up while aiming. Aiming from the
  pawn misses by that offset, most at close range. The right stick turns the
  view only past a magnitude of about 11000; below that an aim error stalls at
  700-850 units.
- WorldInfo `+0x320` heads the navigation point list (1150 points on
  sp_prison_p), linked through point `+0x208`. Point `+0x1DC` is a TArray of
  ReachSpec pointers (vtable `0x820DF328`); ReachSpec `+0x40` is its length,
  `+0x44` its start point and `+0x48` its end point: the lengths matched the
  points' spacing (157 and 196 units); after the yard's first firefight one
  path's end was null. ReachSpec classes by vtable: `0x820DF328` plain walks
  (2772 of 2940 paths with 645 points loaded), `0x820DF898` mantles joining the
  cover slots either side of low cover (52), and `0x820DF980` (104) and
  `0x820DF5E0` (12), both from cover slots, not yet understood. Point vtable
  `0x820955C0` (432) is at cover (z 244 against 186 for `0x820952E8` path
  nodes). A mantle is crossed with the stick toward the far side, A to take
  cover, then A again. `runtime/titles/gears1/navigation_probe.*` serves the
  graph at `GET /api/navigation`.
- `0x82C0C8CC` is the name table, a TArray of name-entry pointers (33,754
  names in sp_prison_p); an entry's `+0x0` is its index and `+0x10` its
  UTF-16BE text, and an object's own name is the (index, number) pair at
  `+0x2C`. `0x82BFC5A4` is a TArray of 100,644 pointers, probably the object
  table. A level's `+0x3C` is its actor array: 412 actors, including
  `WarDroppedPickup`s lying where drones died (collision centre z 131-133).
  Walking onto one and pressing X refilled ammunition. A death shows "Objective Failed" with Load Last Checkpoint
  selected, and A reloads the last checkpoint.

`tools/combat_route.py` steers by this chain from where the gameplay walk
leaves Marcus (in cover near (-760, 1190); the walk's fixed timing varies it by
tens of units) to the yard's first cover and fires there, through
(-922, 1124), (-1002, 1420), and (-1123, 1936) to the jammed door
(-1252, 2325). Tutorial prompts hold Marcus until their button is held under
them, and each appears some seconds after its trigger, so the route answers a
prompt only when the player stalls. At the door the objectives prompt (LB)
follows about 20 s of radio dialogue; the kick (X) is offered only after LB is
released, and an X pressed while the objectives display is still closing is
lost. A run whose walk stopped at the "Choose path" prompt stays at (-317, 1106)
until LT takes the combat path. The cell room and yard follow (-1253, 2637), (-1669, 2727),
(-1752, 3104), (-1377, 3350), and (-1034, 3530) to cover at (-1034, 3640),
where the cover prompt (A) holds the trigger and the first drones engage. The
points-of-interest prompt (Y) near (-1017, 1360) held Marcus in one live run
and not in later ones. The yard checkpoint respawns at (-1606, 2573). The route
fights from that cover: it aims from the camera at each hostile the pawn list
names, first in cover and then with LT, fires half-second bursts, and returns to
cover below 280 health. It cleared the four drones in live runs, sometimes after
reloading the checkpoint once or twice. Dom then walks to about (-1447, 6504)
and radios "Six-Four, we are moving to your location"; Marcus's straight walk
toward him from the yard cover is blocked by the cover and then by a wall.

The ignored `build/ghidra/gears` project imports the loaded image linearly, so
its addresses agree with the runtime: `0x8222E868` reads `7d8802a6`. The
separate ignored `build/ghidra-mapped/gears` import re-lays sections by
VirtualAddress and is distrusted for every address from `.text` on. Ghidra
callers and decompilation still need a completed auto-analysis of the linear
import before they count as evidence.

The title-neutral UE3 contract layer is now grounded separately: Gears consumes
the pinned `shared/x360ue3` binding-schema, frame-lifetime, and semantic-RHI
contracts through an asset-free test. This does not advance the real-image
frontier until those contracts are composed with authenticated XEX imports and
the live native RHI.
