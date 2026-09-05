# GearsUE3 architecture

GearsUE3 is a native/dynarec engine port for the Xbox 360 Gears family. The
user's authenticated executable remains the gameplay authority. Native owners
replace proven subsystems; every other guest path executes through Xenia under
`shared/x360port`.

```text
Gears title/revision adapters + native GearsUE3 systems
                         |
                         v
            future shared/x360ue3
                         |
                         v
              shared/x360port -> Xenia
```

`x360port` currently owns authenticated module loading, Xenia memory/context
lifetime, x64/A64 backend selection, typed imports, and guest register calls.
It will also own code-cache invalidation, device callbacks, native overrides,
scoped original calls, bounded exits, and selection telemetry as those contracts
gain executing evidence. Dynarec is the default. A bounded interpreter fallback is allowed only for a
compilation failure, unsupported guest instruction, or unsafe generated host
block. It must record its reason, transitions, and block count. Explicit
interpreter mode is diagnostic-only; fallback execution cannot satisfy gameplay
or performance evidence.

The future `x360ue3` owns only independently authored UE3-on-Xbox interfaces
proven from a running title: versioned ABIs, RHI semantic operations, binding
schemas, and object/resource/thread/frame lifetimes. It contains no Gears
address, hash, navigation, pass policy, or gameplay behavior. The local
`shared/ue3` tree is reference material only and is never compiled, copied,
linked, packaged, or required.

This repository owns Gears-family native subsystems and exact title adapters.
Gears 1 is the active conformance target. Its asset-free discriminator maps an
aligned synthetic image whose code and entry point use retained leaf address
`0x8222E868`, translates it through Xenia, calls a typed `DbgPrint` binding, and returns to native code. The next
real-image discriminator must execute the original body, a native override, and
a scoped original call, followed by controlled executable-cache invalidation
and one forced safe fallback case. Neither discriminator proves boot or gameplay.

## Runtime invariants

- Exact container and normalized-image identity activate title policy; unknown,
  duplicate, malformed, or ambiguous input refuses.
- Guest addresses, hashes, save namespace, navigation, and native bindings stay
  in the exact title adapter.
- A guest present number remains unchanged through submission, GPU completion,
  scan-out, and presentation. Dropping pending work may not invent or regress
  identity.
- Native overrides have explicit ABI and guest-memory contracts. A scoped
  original call suppresses only its current override and re-enters Xenia at the
  original guest address.
- Renderer replacement requires same-run command, resource, lifetime,
  synchronization, shader, resolve, presentation, and pixel evidence.
- Compatibility and performance reports disclose fallback counts. Any nonzero
  fallback invalidates performance evidence and cannot alone prove gameplay.

## Provisioning and distribution

`./run.sh` is the sole shell entry point and delegates to the locked Python
initializer. A user-owned image or one bounded archive member is extracted into
ignored storage, authenticated, and loaded at runtime. No game bytes, extracted
assets, derived guest source, private engine source, or maintainer-only RE tool
is a shipped or fetched dependency. Builds live under `build/`; disposable
evidence lives under `scratch/`.

Until the authenticated full-image adapter and runtime services are composed
over the pinned `x360port` executor, the named Gears gameplay target and launcher
refuse at that exact boundary. The executor-independent native libraries and
diagnostic tools remain buildable.

## Verification

Agents use a fresh Clang/Ninja build. Focused tests run while editing; the
combined format, lint, structure, migration, distribution, Python, and native
test gate runs once after semantic changes are frozen. Agent-started runtime
runs are headless and silent unless output is the subject of the test.
