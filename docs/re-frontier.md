# Reverse-engineering frontier

This is the ordered evidence chain toward a faithful Gears 1 product. It records
what must be proven next; capability status belongs in `docs/project-state.md`.

1. **Title identity — grounded.** The Gears 1 profile authenticates both the
   original container and normalized executable image. Unknown revisions refuse.
2. **Reusable host semantics — grounded but disconnected.** Native renderer,
   kernel, audio, frame, and wait components have focused tests and retain only
   executable-address/ABI facts. They currently have no guest executor.
3. **Typed Xenia execution boundary — partially grounded.** The exact shared
   `x360port` and maintained Xenia revisions now own memory/module/context
   lifetimes, authenticated synthetic image mapping, JIT calls, and typed imports.
   Device callbacks, bounded exits, native overrides, scoped original calls, and
   executable-state invalidation remain missing shared contracts.
4. **Synthetic Gears-addressed discriminator — grounded.** An asset-free image
   whose code and entry point use `0x8222E868` translates through Xenia, crosses
   a typed `DbgPrint` import into native code, and returns with nonzero translation/emission counts.
   This proves the composition seam, not the real leaf body.
5. **First real guest discriminator — next.** Execute the authenticated original
   leaf at `0x8222E868`, route its real imports, select a native override, suppress
   it for one scoped original call, and observe controlled invalidation.
6. **Fallback discriminator — required with the real discriminator.** Force one safe unsupported
   block through the bounded interpreter fallback, prove reason and counters,
   then prove ordinary execution still selects dynarec. Explicit interpreter mode
   remains diagnostic-only. Fallback results do not satisfy gameplay or performance.
7. **Boot and subsystem restoration — missing.** Reconnect imports, scheduling,
   GPU, audio, input, storage, and frame identity one owned boundary at a time.
8. **Representative interactive gameplay — missing.** Compare deterministic CPU
   state, relevant memory, devices, audio, rendering, and frames with the oracle.
9. **Native renderer completion — missing.** Replace guest command construction
   only after the semantic plan and native backend agree on same-run resources,
   lifetime, synchronization, shader state, resolves, presentation, and pixels.
10. **Platform and performance qualification — missing.** Qualify x86-64, Apple
   Silicon macOS A64, and Android arm64-v8a independently; then measure the native
   8.33 ms renderer target on named hardware.

## Retained exact facts

- Resource AddRef/Release entry points are `0x8222E868` and `0x8222E8E0`.
- The Gears 1 audio-mix operation begins at `0x825F2D40`.
- Normal draw entry points are `0x8222CFF8`, `0x8222D4F8`, `0x8222DA48`, and
  `0x8222DE50`; shader setters are `0x82222808` and `0x82222B98`.
- Shader-state flush `0x822346A8` emits ordered Xenos `IM_LOAD` packets and may
  roll command storage through `0x82221980`.
- The game-authored command-list interpreter at `0x8223B2AC` consumes UE3/Xenos
  command records. This names game behavior, not a CPU execution mode.

These facts must be re-observed through the authenticated Xenia context before
they authorize dispatch or a shared `x360ue3` contract.
