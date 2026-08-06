---
id: 80
title: Two crashes in the headless oracle: a trace/shutdown race, and /dev/shm filled by leaked mappings from earlier crashes
status: resolved
symptom: xenia_oracle aborts with 'free(): invalid pointer' after a frame trace, and later runs take SIGBUS during 'Initializing Processor'
tags: oracle,xenia,crash,trace,shm,workspace
created: 2026-08-06
updated: 2026-08-06
---

Two independent defects, found together because the first one's crashes caused
the second one.

## 1. The trace/shutdown race -- "free(): invalid pointer"

Backtrace off the core, which named it in one read:

    fwrite -> _IO_file_xsputn -> _IO_file_overflow -> _IO_free_backup_area
      -> malloc_printerr -> abort
    snappy::Compress
    TraceWriter::WriteMemoryCommand
    VulkanSharedMemory::InitializeTraceCompleteDownloads
    VulkanCommandProcessor::ExecutePacketType3
    CommandProcessor::WorkerThreadMain          <-- the GPU worker thread

ROOT CAUSE. `RequestFrameTrace` is fire-and-forget with **no completion signal
of any kind**. The trace opens at one swap and closes at the NEXT
(`pm4_command_processor_implement.h`, the `PM4_XE_SWAP` case), and everything in
between is written from the GPU worker thread. The oracle's main thread called
`emulator.reset()` as soon as its capture loop ended, which closes the trace
writer's `FILE*` under a thread sitting inside `fwrite`.

The visible damage was not the abort, it was the FILE: a 4.4 MB fragment of a
42 MB trace, which loads, plays back, and looks like a trace of a frame that did
almost nothing. That is what "the oracle's trace only had 2 draws" was.

FIX, in two parts, both of them the mechanism rather than the timing:

  * `CommandProcessor::is_frame_trace_pending()` -- an atomic set when the
    request is accepted and released only after `trace_writer_.Close()`. There
    was no way for any caller to know, which is why the race was unavoidable
    rather than unlucky. Exposed as `GraphicsSystem::IsFrameTracePending`.
  * `tools/xenia_oracle` waits on it, bounded at 60 s, before `emulator.reset()`,
    and LOGS WHICH WAY IT WENT. A trace that never completes is a real outcome
    -- the request is served at a swap, so a title that is loading never serves
    one (measured: a request at 35 s produced no file at all, and the run had
    2 draws a frame) -- and it must not be silent.

NOT a fix: capturing the trace earlier so the run has margin. That was the first
thing tried and it is a workaround; it leaves the race in place for anyone who
picks a different number.

## 2. /dev/shm filled by the debris of those crashes -- SIGBUS at startup

After the abort above, two consecutive runs died with SIGBUS three log lines in,
at `Setup: Initializing Processor`. That is not the guest and not a code change;
it is the tmpfs quota:

    quota -s   ->   tmpfs  6355M*  6355M  6355M     (at the limit, starred)
    /dev/shm   ->   43 x xenia_memory_*      (4.6 GB each, sparse)
                    43 x xenia_code_cache_*  (256 MB each)

None held by any process, going back to 05 Aug 01:13 -- so this had been
accumulating across sessions, not just this one.

ROOT CAUSE. `memory_posix.cc` created these with `shm_open` and cleaned them up
with `std::atexit` / `std::at_quick_exit` handlers, which do not run on SIGSEGV,
SIGBUS or SIGABRT -- i.e. exactly the cases that leak. Every crashed run left
both objects behind, and once the quota was full the next run's write to its own
code cache took SIGBUS. A crash caused entirely by the debris of earlier
crashes.

FIX: `shm_unlink` the name IMMEDIATELY after `shm_open` + `ftruncate`, while
holding the fd. The mapping is kept alive by the descriptor, so nothing changes
functionally, and the object is now impossible to leak however the process dies.
Checked first that nothing reopens these by name -- every caller maps through
the returned handle (`memory.cc`, `code_cache_base.h`, and the tests). The
atexit machinery and its tracking vector are deleted rather than left beside the
new code.

## For next time

`df` says 76% and looks fine while `quota -s` says the user is AT the limit --
the tmpfs is shared and the per-user quota is what bites. On any unexplained
SIGBUS from a Xenia-derived binary, check `quota -s` and `ls /dev/shm` first.

### Note (2026-08-06)
## Verified, and one thing NOT fixed (2026-08-06)

Re-ran the exact configuration that aborted (trace requested at 200 s, run ends
at 210 s):

    oracle: requesting a Xenia frame trace at 210s
    oracle: the frame trace finished writing after 1s
    4D5307D5_14660.xtr   41,617,348 bytes      <- complete, was a 4.4 MB fragment
    /dev/shm             0 xenia objects       <- was 2 leaked per run

So both fixes hold: no abort, a whole trace, and nothing left in shared memory.

NOT FIXED, and it must not be read as fixed: **the process still does not exit
on its own.** The run ended on the 420 s `timeout` (exit 124), so teardown hangs
somewhere after the trace wait returns. The leak fix covers this case -- the
objects are unlinked at creation, so even a SIGKILL leaves /dev/shm clean -- and
the trace is safely on disk before it, so nothing is lost. But a run still needs
an external timeout to end, and that is a separate defect nobody has looked at.

Every oracle invocation should therefore keep its `timeout` wrapper, and it is a
workaround, not a solution.
