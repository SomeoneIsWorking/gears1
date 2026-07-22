---
id: 8
title: Differential harness design: comparing our runtime against Xenia
status: investigating
symptom: need to find the first divergence between our recompiled runtime and a reference emulator for a given guest execution
tags: harness,method
created: 2026-07-22
updated: 2026-07-22
---

## Root cause


## What was tried / dead ends


## Resolution

### Note (2026-07-22)
Full ITRACE/DTRACE instruction tracing is NOT the cheapest correlation. Xenia exposes conditional guest breakpoints directly as cvars: --break_on_instruction=<guest address> (int3 before that address executes), with --break_condition_gpr=<n>, --break_condition_value=<v>, --break_condition_op=<eq|...>, --break_condition_truncate. Source: src/xenia/cpu/cpu_flags.cc:51-56.

### Note (2026-07-22)
That matches how we already debug our own runtime -- conditional breakpoints on a guest address with a register predicate -- so the two sides can be compared at the SAME guest instruction with the SAME condition, without correlating two giant traces. Guest addresses are identical across both because both run the same XEX; only host addresses differ.

### Note (2026-07-22)
First concrete comparison to run: break both sides at the float store in sub_82761CA8 (guest 0x82761D64, stfs f0,0(r9)) and compare r9/r29/r30. If Xenia performs the same store to a list head and survives, the divergence is downstream in how we service it; if Xenia never performs it, the divergence is upstream and the reported-values audit resumes with a reference to check against.

### Note (2026-07-22)
BLOCKER on the gdb-based comparison: running xenia_canary under gdb segfaults inside the AMD Vulkan driver (radv_BindImageMemory2 via xe::ui::vulkan::util::CreateDedicatedAllocationImage) before the guest breakpoint is reached. It is a gdb/ptrace + radv interaction, NOT a Xenia fault -- the same binary runs Gears cleanly with zero errors when not under gdb. --gpu=null --apu=nop does not avoid it, because --headless still creates a VulkanPresenter and its swapchain.

### Note (2026-07-22)
Ways round it, untried, cheapest first: (a) let break_on_instruction's int3 dump core and inspect the core rather than attaching gdb live; (b) run under a virtual display / offscreen so the presenter takes a different path; (c) skip breakpoints entirely and use Xenia's own instrumentation -- trace_functions, trace_function_data, disassemble_functions -- which needs no debugger; (d) build Xenia with the presenter disabled. Option (c) is probably best: it avoids the debugger entirely and its output is already guest-side.
