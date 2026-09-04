# Xenia reuse boundary

`shared/x360port` is the sole Xbox 360 execution owner. Gears pins an exact local
revision and currently consumes only its authenticated image/import validation
target. The missing product target must wrap Xenia's existing `Memory`, `Processor`,
`ThreadState`, `RawModule`, x64/A64 translators, and code cache rather than creating
a second Xenon CPU implementation.

The product contract must expose:

- typed execution context, guest memory, imports, device callbacks, and exits;
- native override selection and a scoped call to the original guest address;
- executable-state invalidation owned by Xenia's cache;
- dynarec-first selection with a bounded interpreter fallback only for compilation
  failure, unsupported instructions, or unsafe generated host execution;
- reason-labelled fallback counters and an explicit diagnostic-only interpreter mode;
- independent x86-64, Apple Silicon macOS A64, and Android arm64-v8a qualification.

Fallback coverage is not gameplay or performance evidence. Normal gameplay and all
performance gates must report dynarec selection and nonzero translated blocks.

Gears owns exact title hashes, addresses, native subsystem policy, renderer behavior,
and conformance scenarios. The future `shared/x360ue3` may own only independently
authored UE3/Xbox interfaces that are proven from the executing boundary; it must not
be created as an empty framework or use the local `shared/ue3` reference source.
