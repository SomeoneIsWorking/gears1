---
id: 48
title: stwcx. is translated as a value CAS, not a store-conditional -- audited, and it cannot affect this title
status: resolved
symptom: cross-thread guest state corrupts; FRingBuffer ReadPointer leaves a command boundary; suspected atomics mistranslation
tags: recompiler,atomics,lwarx,stwcx,memory-model,ringbuffer,ABA
created: 2026-07-29
updated: 2026-07-29
---

## Question

Does XenonRecomp's translation of the PowerPC atomic primitives diverge from
Xenon hardware in a way that can corrupt cross-thread guest code -- in
particular UE3's FRingBuffer at guest 0x82C0CB24, whose ReadPointer walks off a
command boundary (see the ring-drain crash entry)?

## What each primitive actually emits

All quotes from `extern/XenonRecomp/XenonRecomp/recompiler.cpp` @ be68c70.

| PPC | emitted C++ | verdict |
|---|---|---|
| `lwsync` (:1370) | `__atomic_thread_fence(__ATOMIC_ACQ_REL);` | FAITHFUL -- lwsync orders everything but StoreLoad, which is exactly an acq_rel fence. |
| `sync` (:1883) | `__atomic_thread_fence(__ATOMIC_SEQ_CST);` | FAITHFUL -- lowers to `mfence`, supplying the StoreLoad order x86-64 TSO lacks. |
| `isync` (:1001) | `__atomic_thread_fence(__ATOMIC_ACQUIRE);` | FAITHFUL for the `branch;isync` acquire idiom. |
| `eieio` (:995) | `__atomic_thread_fence(__ATOMIC_ACQ_REL);` | STRONGER (eieio is StoreStore on caching-inhibited memory only). Safe. |
| `lwarx` (:1348) | `ctx.reserved.u32 = *(uint32_t*)(base + ...); rD.u64 = __builtin_bswap32(ctx.reserved.u32);` | Value correct (BE bytes kept raw in `reserved`, zero-extended into rD). **Load was NOT volatile** -- fixed, see below. |
| `ldarx` (:1204) | same shape, `uint64_t` / `bswap64` | same. |
| `stwcx.` (:1828) | `cr0.lt=0; cr0.gt=0; cr0.eq = __sync_bool_compare_and_swap((uint32_t*)(base+...), ctx.reserved.s32, __builtin_bswap32(rS.s32)); cr0.so = xer.so;` | WEAKER: a **value** CAS, not a store-conditional. Also STRONGER in that `__sync_bool_compare_and_swap` is a full barrier and can never fail spuriously. |
| `stdcx.` (:1673) | same, 64-bit | same. |

`ctx.reserved` is a `PPCRegister` field of `PPCContext` (ppc_context.h:358), and
`PPCContext` is a stack local of each guest thread entry
(`runtime/kernel_thread.cpp:103`), so the reservation register is genuinely
per-thread. `config/gears.toml:23` sets `reserved_as_local = false`, so all
sites use `ctx.reserved` -- there is no function-local-variable variant in play
that could break a larx/stcx. pair split across functions.

Unhandled opcodes `return false` (recompiler.cpp:2545), so nothing atomic is
being silently dropped. The seven mnemonics above are the complete set present
in the recompiled output.

## The ABA hole, and why it cannot bite this title

The real divergence is that a value CAS answers "does it still hold the value I
read" while hardware `stwcx.` answers "has anything touched this line since my
`lwarx`". They differ exactly when a value is changed and changed back.

Measured, not assumed. `tools/atomic_audit.py` walks every
`ppc_recomp.*.cpp`, pairs each `larx` with its `stcx.`, and classifies the
window by what the body does. Real output over `scratch/ppc`:

```
windows found: 184  benign(pure RMW): 143  suspect: 41  orphan/unpaired: 0

=== BENIGN body-op histogram ===
     74  addi
     27  add
```

- **143 windows are pure read-modify-write** -- the body contains *only* `add`
  or `addi`. These are `InterlockedIncrement` / `Decrement` / `ExchangeAdd`.
  The stored value is `f(loaded)` for an associative `f`, so a hardware retry
  after an ABA converges to the same final value the non-retrying CAS produces.
  Provably equivalent.
- **40 of the 41 "suspect" windows are the guest's own compare-exchange**, all
  the identical shape `lwarx rD; cmpw rD,rOld; bne fail; stwcx. rNew`. The guest
  has *already* reduced the question to a value comparison and throws the
  reservation information away. A value CAS is precisely the intended semantics.
- **The 1 remaining window is `sub_82AA37C0` (`ppc_recomp.172.cpp:54051`)** --
  a 64-bit `ldarx / stw old-head into new-entry / lwsync / bump both a Depth and
  a 16-bit Sequence / rldimi new pointer into the high half / stdcx.`. This is
  the XDK `InterlockedPushEntrySList` tagged-pointer head. It defeats ABA *by
  its own construction* (the sequence counter is incremented on every push), so
  a 64-bit value CAS is exactly correct here too.

The 35 `stwcx.` that have no `larx` of their own are the documented
clear-the-reservation idiom on the CAS mismatch path. Verified mechanically:

```
unpaired stcx. storing back the larx destination reg (clear-reservation idiom): 35
unpaired stcx. storing something ELSE: 0
```

They translate to a CAS of the value to itself, which is a no-op whether it
succeeds or fails, and `cr0.eq` is not consumed on that path.

**Conclusion: no reservation window in Gears of War is ABA-sensitive. The CAS
translation of `stwcx.`/`stdcx.` cannot produce a divergence in this title.**

## FRingBuffer uses no atomics at all -- the CAS translation is IRRELEVANT to it

The allocator `sub_8221CBA8` (`ppc_recomp.15.cpp:16682`) and the drain loop
`sub_82444EF0` (`ppc_recomp.60.cpp:23359`) were read instruction by instruction.
Both contain **zero** `lwarx`/`stwcx.`/`lwsync`/`sync`/`eieio` -- only plain
`lwz`/`stw` on the ring fields (`+0` base, `+8` write pointer, `+12` wrap
marker, `+20` read pointer). Producer and consumer synchronise by plain
single-word loads and stores alone.

That is safe under the current translation for a reason worth writing down:
`PPC_LOAD_U32`/`PPC_STORE_U32` (ppc_context.h:44,80) are **volatile** accesses,
so the producer's spin at `loc_8221CBC0` genuinely re-reads the consumer's
pointer each iteration instead of being hoisted, and x86-64 TSO supplies the
acquire/release the guest's plain accesses relied on the PPC's (absent) fences
to *not* need.

So: whatever is moving `ReadPointer` off a command boundary, it is **not** the
atomics translation. Look elsewhere -- at the command sizes returned by the
per-command virtual call at `loc_82444F4C` (`bctrl` on `[[r30+0]+4]`, whose
result `r29` is added to ReadPointer), or at the wrap handling against `+12`.

## What was actually fixed

`lwarx`/`ldarx` were the **only** guest memory reads in the whole translation
that were not `volatile`, unlike every other load (`PPC_LOAD_*`). That is a C++
data race on a non-atomic object, and it left the optimiser free to CSE, hoist
or duplicate the one load that must be re-executed every retry. It survives
today only because the paired `__sync_bool_compare_and_swap` happens to be a
full barrier that pins the load inside the loop -- an incidental property, not a
guarantee. Made `volatile`.

Verified by rebuilding XenonRecomp and regenerating the whole guest into
`scratch/ppc_verify`, then diffing against `scratch/ppc`: the recompiler-caused
delta is **exactly 184 lines** (181 `lwarx` + 3 `ldarx`), matching the
instruction counts; every other differing line is `tools/prepare_overrides.py`
post-processing already applied to the operator's tree.

## What was deliberately NOT fixed, and the residual risk

A faithful store-conditional needs the reservation to be *lost when any thread
stores to the granule*. Guest stores go through plain `PPC_STORE_*` with no
hook, so modelling it means putting a reservation-table update on the hot path
of every guest store in the game. A partial model (self-invalidation only)
would leave the real gap open and is a half-fix. Not done.

**Residual risk, stated precisely:** if guest code is ever added or reached that
performs a `larx`/`stcx.` on a location whose *meaning* can revert to a prior
value while its referent changed -- an untagged lock-free LIFO/queue head, or a
generation-less pointer swap -- it will silently succeed where hardware would
have retried. `tools/atomic_audit.py` is the detector: re-run it after any
recompiler config or image change and confirm the suspect list still contains
only `cmpw`-shaped guest CAS plus the tagged SList.

## Not determined

- Whether `__atomic_thread_fence` formally orders `volatile` (non-atomic)
  accesses. In the C++ memory model it does not; in practice clang lowers it to
  an LLVM `fence`, which does block movement of ordinary and volatile memory
  ops. This is the standard assumption for this class of emulator and no
  evidence of it failing was found, but it is an assumption, not a proof.
- Whether the 35 clear-reservation `stwcx.` sites' CAS-to-self can be observed
  by any guest code. Argued unobservable (an atomic store of the value already
  present); not empirically demonstrated.
