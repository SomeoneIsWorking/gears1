# 2026-07-22 — First real recompilation pass

Goal: get XenonRecomp to actually emit C++ for Gears of War's XEX, and report
honestly what works and what does not.

## Verified from the previous session

All of these were re-checked against real data, not taken on trust:

- `scratch/bin/default.xex` — 4,767,744 bytes, header magic `XEX2`. Valid.
- `tools/xex_probe` — decrypts/decompresses the XEX and dumps
  `scratch/raw/gears_image.bin` (13,500,416 bytes, load base `0x82000000`,
  entry `0x82612BF0`). The dump starts with `MZ`, i.e. a real PE image, so
  decryption/decompression is genuinely working.
- 17 sections resolved, `.text` at `0x82170000` size `0x956FE4` (~9.7 MB) —
  consistent with a UE3 title.
- 226 imports resolved by name.
- Register save/restore helpers located by byte-pattern scan (the eight
  addresses now in `config/gears.toml`).
- `scratch/config/gears_switch_tables.toml` — 360 jump tables from XenonAnalyse.

## Commands

```
./scratch/build-xenonrecomp/XenonRecomp/XenonRecomp \
    config/gears.toml extern/XenonRecomp/XenonUtils/ppc_context.h
```

Paths in `config/gears.toml` are relative to the config file and point into
`scratch/`. The XEX is never copied into the repo.

## Result: it recompiles

- 49,012 functions emitted, 194 `ppc_recomp.*.cpp` files, ~176 MB of C++.
- Runtime ~4 s.

## What was broken, and the fix

### Unimplemented instructions were silently dropped (fixed)

The first pass logged **3,394 unrecognized instructions across 36 distinct
mnemonics**. The root cause was not the missing implementations themselves but
`recompiler.cpp` discarding the return value of `Recompile(functions[i])`: an
instruction with no implementation emitted **only a comment**, e.g.

```c
	// vslh v2,v11,v8
	// lvlx v3,0,r8          <-- next instruction, reads a stale v2
```

so the function still got emitted and the following code ran against stale
registers. A silent miscompile, not a build error. Blast radius was 129 of
49,012 functions (0.26%) — small, but invisible.

Fixed in the fork (`extern/XenonRecomp`, branch `gears`, commit `9393c78`):

1. An instruction with no implementation now emits `__builtin_debugtrap()` and
   the tool exits non-zero with a count. The gap fails loudly instead of
   quietly producing wrong values.
2. Implemented the 36 mnemonics Gears actually uses — mostly halfword and
   saturating VMX forms whose word-width siblings already existed — plus the
   missing RC-bit handling on `vcmpgtuh.`.

**Verified:** unrecognized count 3394 → **0**, RC-bit warnings 6 → **0**, exit
code 0. Spot-checked the emitted code: `vslh` now expands to eight per-lane
shifts, and `cror 4*cr1+eq,lt,4*cr6+eq` expands to
`ctx.cr1.eq = ctx.cr0.lt | ctx.cr6.eq` (correct bit→field/sub-field mapping).

The 3 `__builtin_debugtrap()` calls left in the output predate this work and
come from other paths; they are not unimplemented instructions.

### Jump tables escaping function boundaries (NOT fixed)

**1,394** errors of the form:

```
ERROR: Switch case at 82XXXXXX is trying to jump outside function: 82YYYYYY
```

This is the known limitation called out in XenonRecomp's README: the function
boundary analyser treats a jump table as a tail call, so it cuts functions
short and the switch targets then land outside the function it thinks it is in.
Not yet investigated. This is the next thing to work on.

## Threading assessment

UE3 is heavily multithreaded, so this was checked early rather than deferred.

Imports confirm a real threading surface: `ExCreateThread`,
`ExTerminateThread`, `NtResumeThread`/`NtSuspendThread`, `KeSetAffinityThread`,
`KeSetBasePriorityThread`, `KeTlsAlloc`/`Get`/`Set`/`Free`,
`KeWaitForSingleObject`/`KeWaitForMultipleObjects`,
`NtWaitForMultipleObjectsEx`, `RtlEnterCriticalSection` and friends,
`KfAcquireSpinLock`/`KeAcquireSpinLockAtRaisedIrql`, `KeInitializeSemaphore`,
`NtCreateMutant`, `NtCreateEvent`/`NtSetEvent`/`NtPulseEvent`,
`InterlockedPopEntrySList`/`InterlockedFlushSList`.

Structurally XenonRecomp is **better placed than expected**. `PPCContext` is
passed as a parameter to every emitted function, so one context per guest
thread needs no recompiler change. `lwarx`/`stwcx.` lower to a real
`__sync_bool_compare_and_swap`, so guest atomics work.

The real hole is **memory barriers**. `sync`, `lwsync`, `eieio` and `isync` are
all emitted as no-ops (`recompiler.cpp`). Consequences:

- On x86-64 this is *mostly* survivable: TSO gives acquire/release ordering for
  free, which covers most `lwsync` uses. Loads and stores are marked volatile,
  which blocks Clang from reordering them against each other.
- It is **not** fully safe. x86-64 does not provide StoreLoad ordering, which
  is exactly what the heavyweight `sync` is for. Any guest code relying on
  `sync` for a store-then-load handshake can break.
- On ARM64 dropping these would be outright broken.

`KeSetAffinityThread` is a second question: the game pins work to specific Xenon
hardware threads. The runtime will have to decide whether to honour that or map
it to nothing.

None of this is a blocker for the recompiler stage, but the barrier no-ops are
a real correctness debt that has to be paid before trusting multithreaded
output. Not yet fixed — recorded here rather than patched over.

## Next steps

1. Root-cause the 1,394 jump-table/function-boundary errors.
2. Decide on memory barrier emission (at minimum make `sync` a real
   `atomic_thread_fence(seq_cst)` and measure the cost).
3. There is still no runtime. Nothing has been *executed* — "it recompiles" is
   a statement about code generation only, not about correctness of the output.
