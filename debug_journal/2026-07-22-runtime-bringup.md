# 2026-07-22 — Runtime bring-up summary

This is the durable, distribution-safe summary of the initial runtime bring-up.
Detailed forensic transcripts were removed because they embedded material derived
from the game executable and are not needed to build or understand the clean
runtime.

## Result

The generated guest code compiled with Clang and entered the title's startup
path. The first host runtime established the guest address space, loaded the
executable, installed the recompiled-function dispatch table, created the main
guest thread context, and invoked the title entry point.

## Root causes fixed during bring-up

- The executable loader recorded function imports but discarded variable
  imports. The loader now exposes both kinds and the runtime supplies guest
  storage for imported variables.
- The initial guest thread had no console thread-control block. The runtime now
  creates the required per-thread structures, stack, TLS, and current-thread
  linkage before entering guest code.
- Guest memory barriers were initially no-ops. They were implemented before
  enabling concurrent guest threads.
- Physical-memory aliases were not mapped coherently. The runtime now preserves
  the console-visible aliases over shared backing storage.
- Kernel dispatcher objects, handles, waits, threads, and file I/O were added as
  real host-backed services. Unsupported imports fail loudly rather than return
  invented success.

## Verification lessons

- A generated instruction being accepted by the recompiler is not proof that
  its semantics are correct. Focused instruction tests and mutation checks were
  added for the high-risk vector, shift, conversion, and condition-register
  operations encountered during startup.
- Plausible pointer failures cannot be diagnosed reliably by following one
  inferred chain through generated code. The project moved toward checked guest
  calls, bounded memory probes, reproducible headless runs, and a differential
  oracle.
- Null graphics, audio, or filesystem behavior must be labelled as such. A
  successful boot through a null service proves only control-flow progress.

## Current standing

This journal is historical. Current subsystem ownership and verified coverage
live in `docs/codemap.md`; ordered reverse-engineering progress lives in
`docs/re-frontier.md`; active symptoms and root causes live in `docs/issues/`.
