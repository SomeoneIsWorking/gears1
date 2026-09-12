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
   real `0x82233668` body, proving its result, native override/scoped original
   path, and both reported-write and automatic virtual-write invalidation. The
   authenticated PE's `0x822336C0` call returns to `0x82233668` when a resource
   first gains a reference with type nibble 4, flag `0x40000000`, and a linked
   resource at offset 24. A headless real-image fixture proves that call enters
   the native override and that removal restores the original nested call.
   The inspector resolves the two XEX library-table entries by index, retaining
   `xam.xex` and `xboxkrnl.exe` as distinct bindings; service semantics are still
   mostly unimplemented. The title-owned `XGetAVPack` ordinal 971 and shared
   `XamInputGetCapabilities`/`XamInputGetState` ordinals 400/401 bindings are
   proven through their real thunks.
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

- Resource AddRef/Release entry points are `0x82233668` and `0x822336E0`.
- The Gears 1 audio-mix operation begins at `0x825F2D40`.
- Normal draw entry points are `0x8222CFF8`, `0x8222D4F8`, `0x8222DA48`, and
  `0x8222DE50`; shader setters are `0x82222808` and `0x82222B98`.
- Shader-state flush `0x822346A8` emits ordered Xenos `IM_LOAD` packets and may
  roll command storage through `0x82221980`.
- The game-authored command-list interpreter at `0x8223B2AC` consumes UE3/Xenos
  command records. This names game behavior, not a CPU execution mode.

These facts must be re-observed through the authenticated Xenia context before
they authorize dispatch or a shared `x360ue3` contract.

The old ignored `build/ghidra/gears` project imported the normalized PE as a
linear raw image: at guest VA `0x82233668` it reads raw file offset `0x233668`
(`954b0004`) instead of the authenticated `.text` section's offset `0x22e868`
(`7d8802a6`). Its zero-reference and VA-based disassembly results are
distrusted. The separate ignored `build/ghidra-mapped/gears` PE import returns
the latter bytes and `4bffffa9` at `0x822336C0`; full autoanalysis did not
complete, so its reference database is not yet evidence. Use mapped PE bytes
or the running Xenia discriminator for exact VAs; Ghidra callers/decompilation
need a completed, byte-checked mapped analysis.

The title-neutral UE3 contract layer is now grounded separately: Gears consumes
the pinned `shared/x360ue3` binding-schema, frame-lifetime, and semantic-RHI
contracts through an asset-free test. This does not advance the real-image
frontier until those contracts are composed with authenticated XEX imports and
the live native RHI.
