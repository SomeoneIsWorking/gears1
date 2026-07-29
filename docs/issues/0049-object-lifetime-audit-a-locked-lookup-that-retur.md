---
id: 49
title: Object-lifetime audit: a locked lookup that returns a raw pointer outlives its lock
status: resolved
symptom: second-order crash on an unrelated thread; use-after-free in the XMA decoder, a file handle, or a runtime static during teardown
tags: lifetime,threading,use-after-free,audit,xma,teardown
created: 2026-07-29
updated: 2026-07-29
---

## The class

A lookup takes a table mutex, returns a RAW pointer or reference into the
container, and releases the lock before the caller is done with it. Another
thread then erases/destroys that entry. The first instance found was `FindFile`
in `runtime/kernel_file.cpp` (fixed earlier: it now hands out a `shared_ptr`).
This is an audit of the rest of the runtime for the same shape, plus the
teardown-ordering variant.

## Detector (validated -- it fires on the pre-fix FindFile)

Two greps over `runtime/*.cpp`:

- A: `^[type] *\**name(` -- functions whose return type is a raw pointer/reference.
- B: `return .*\.get()|return \*[a-z]|return &` -- returning a borrowed handle
  out of a function body.

Both were run against a reconstruction of the pre-fix `FindFile`
(`OpenFile* FindFile(uint32_t)` returning `it->second.get()` under a
`lock_guard`) and both matched it. A detector that cannot produce the positive
result reports zero and that reads as an answer, so this check came first.

## Found and fixed

1. **`runtime/xma.cpp:156` `HwContext()`** -- the surviving instance of the exact
   `FindFile` shape. It returned `XmaHwContext&` out from under
   `g_hwContextMutex`; the caller then spends a whole synchronous XMA decode in
   `Work()`/`Clear()`. `ReleaseXmaContext` (same file) `.reset()`s the slot under
   that same mutex, destroying the `XmaHwContext` -- its ffmpeg decoder and
   output ring -- while another thread decodes through it. `XMAReleaseContext`
   is a guest call (`runtime/xaudio_null.cpp:532`) on whatever thread the title
   makes it from, and kicks demonstrably arrive on more than one thread (that is
   what `g_kicksFromPump` counts). FIX: `std::array<std::shared_ptr<...>>` and
   `HwContext()` returns a `shared_ptr`. `Work`/`Clear`/`Release` already all take
   `XmaHwContext::mutex_`, so with the object kept alive they simply serialise.

2. **`runtime/main.cpp`** -- teardown ordering, the more reachable bug. On the
   guest entry point returning, main did `memory.Release(); return EXIT_SUCCESS;`
   with every ExCreateThread thread DETACHED and three runtime service threads
   (tick publisher, timer scheduler, stall detector) still running and unjoined.
   That munmaps the guest address space out from under them and then runs every
   static destructor -- the two heaps, the critical-section and spin-lock tables,
   the handle and open-file tables. This is exactly what `runtime/fatal_exit.h`
   was written for (ASan: heap-use-after-free in `HostLockFor` on the audio pump,
   freed by `~unordered_map` from `__run_exit_handlers`), except that file only
   covered the `KeBugCheck` path and left the NORMAL end of a run uncovered.
   FIX: `gears::FatalExit(EXIT_SUCCESS)`.

3. **`runtime/xam_user.cpp:519`** -- the debug line for a new content enumerator
   read `g_enumerators[handle]->items.size()` AFTER releasing
   `g_enumeratorsMutex`: an unsynchronised touch of the map while another thread
   may be inserting, and `std::map::operator[]` is mutating, so not even a benign
   read. FIX: take the count off the enumerator before the move.

## Checked and NOT the bug (so nobody re-walks these)

- `HostLockFor` in `kernel_spinlock.cpp:18` and `kernel_sync.cpp:32` both return
  `mutex&` out from under a table lock, but nothing ever erases from those tables
  and the `unique_ptr` indirection keeps the mutex fixed across a rehash. Safe by
  construction, not by accident -- but only as long as the tables stay
  append-only.
- `wait_probe.cpp` `FindProgress`/`FindSite`/`MyThreadState` return interior
  pointers into a fixed-size array or a never-erased vector of `unique_ptr`.
- `kernel_timer.cpp` `Scheduler()` holds a `min_element` iterator across an
  `erase`, but copies the `shared_ptr` out first and never touches the iterator
  after `lock.unlock()`.
- `kernel_thread.cpp` `ThreadForObject` already returns `shared_ptr`, and
  `g_threadsByObject` is never erased so `t_currentThread` stays valid.
- `guest_heap.cpp`, `kernel_objects.cpp` handle/guest-address tables: all
  `shared_ptr` returns already.

## Could not determine

- `gpu_draw_xlate.cpp:51` `GetAnalyzedShader` returns a raw
  `xe::gpu::SpirvShader*` out of an unsynchronised `static unordered_map`. The
  entries are never erased so the POINTER is stable, but if that cache is ever
  touched from two threads the map itself races. I did not establish that the
  draw path is single-threaded; left alone rather than "fixed" on a guess.
