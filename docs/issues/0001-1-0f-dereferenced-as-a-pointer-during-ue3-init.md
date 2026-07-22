---
id: 1
title: 1.0f dereferenced as a pointer during UE3 init
status: investigating
symptom: SIGSEGV in sub_82766F68 at lwz r30,12(r31) with r31=0x3F800000; boot dies during UE3 initialisation
tags: crash,ue3,memory,blocker
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
Chain fully measured, all from observed writes: the word at block+0x32C is a zero-initialised list head; sub_82761CA8 stores 1.0f over it via r29+36 (count 1, in bounds); sub_826ED298's push-front copies the head into node->next; the walker dereferences it. The list code is correct throughout -- the float store is the intruder.

### Dead end (2026-07-22)
Missing game data -- crash is byte-identical with and without the 6.3 GB extracted disc, same instruction, same addresses.

### Dead end (2026-07-22)
The 1,394 jump-table/function-boundary errors -- none of the functions in the call path appears in the recompiler's switch-escape error list.

### Dead end (2026-07-22)
Overlapping runtime allocations -- 0xA06F0000 is a single 4 KiB block from one MmAllocatePhysicalMemoryEx; both objects live inside it.

### Dead end (2026-07-22)
Forced 64 KiB physical pages -- MmAllocatePhysicalMemoryEx now honours the requested alignment (more correct anyway); crash unchanged.

### Dead end (2026-07-22)
Vblank interrupt never firing -- now driven at 60 Hz on its own guest thread; identical crash and identical progress with GEARS_NO_VBLANK on and off.

### Dead end (2026-07-22)
ObDereferenceObject being a no-op -- called exactly 3 times, balanced with ObReferenceObjectByHandle. Direction was backwards anyway: a no-op deref makes objects live LONGER, and the symptom needs memory reused EARLIER.

### Dead end (2026-07-22)
Recompiler defect in the instructions this depends on -- rlwinm rA,rS,0,0,30 and addi -32 hand-checked against PPC semantics; VMX implementations now covered by mutation-checked tests.

### Dead end (2026-07-22)
Uninitialised/garbage tree link -- the node at 0xA06F034C is written twice with plausible in-block aligned links before the tree reaches it. It is a real node.

### Dead end (2026-07-22)
Volatile-register clobbering of r3 -- r3 is set 146 lines before use, but there are no calls in between, so the value survives legitimately.

### Dead end (2026-07-22)
GuestHeap never reusing freed memory -- 74 allocations, 10 frees before the crash, all frees in the title heap; the physical block is never freed.

### Dead end (2026-07-22)
A pool header inside the block -- scanning the whole 4 KiB block finds no stored copy of 0xA06F0308, so the pointer is computed, not read from bookkeeping.

### Dead end (2026-07-22)
Reported display values steering the layout -- changing the reported mode from 1280x720 to 640x480 gives a byte-identical crash at the same addresses.

### Note (2026-07-22)
NEXT METHOD: stop hypothesis-testing. Build a differential harness against a reference emulator (Xenia ships ITRACE/DTRACE per-instruction tracing with --store_all_context_values). Xenia's Linux support is described upstream as 'extremely experimental and presently incomplete', which is the risk.

### Note (2026-07-22)
GHIDRA RESULT for the float store. sub_82761CA8 is a float-array copy WITH DENORMAL FLUSHING into a container: it calls FUN_82761758(obj, x, flags, 0x78, 0, count) first, then writes count elements starting at obj+0x24, flushing denormals to signed zero as it goes. So obj+0x24 is the container's INLINE value storage and the write is entirely by design -- it is not a stray pointer. That kills the 'the float store is the intruder' framing from earlier.

### Note (2026-07-22)
Which reframes the bug again: the same memory is legitimately a float array to one subsystem and a list head to another, so the object is being REUSED or REPURPOSED while something still holds a list pointer into it. The next question is what FUN_82761758(..., 0x78, ...) does -- it looks like a reserve/capacity call, and if it is supposed to move the storage elsewhere when capacity grows, a wrong answer from our runtime could leave the data inline when it should not be.

### Note (2026-07-22)
FUN_82761758 is NOT a memory reserve. Decompiled, it writes param_3 to obj+0x14, packs bitfields into obj+0x08, and switches on (param_4 - 5) setting component masks like 0xdb6 / 0x924 into obj+0x10. param_4 is 0x78. That is the shape of a GPU shader-constant / vertex-element DESCRIPTOR being configured -- format code plus component write masks -- not an allocation.

### Note (2026-07-22)
So the object is a shader/register descriptor whose inline float storage lives at +0x24, and sub_82761CA8 fills that storage with denormal flushing. This puts the whole conflict inside the GRAPHICS path, which matters because our GPU is an explicit null: no command processor, an inert register file at 0x7FC00000, and VdSwap presenting nothing. If the title pools these descriptors and recycles them based on GPU progress it can observe -- fences, command-buffer completion, register reads -- then a pool that never sees work retire could hand out a descriptor that is still linked elsewhere. That is a concrete, testable link between the null GPU and this crash, and it was NOT visible before decompilation.
