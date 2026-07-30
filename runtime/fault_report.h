// Say something when the process dies.
//
// Six consecutive runs of the crash repro produced a core dump and no output at
// all about the fault. Every hypothesis those runs were meant to test was
// evaluated against a log that simply stopped -- which is indistinguishable
// from a log that never had anything to say. A crash reporter is not a
// nice-to-have on a port: the fault IS the observation, and without one every
// run costs the same and returns nothing.
//
// The handler cannot reach a PPCContext -- the recompiler passes it by
// parameter, so there is no thread-local to read -- but it can reach three
// things that between them name the fault: the address, which guest thread was
// running, and the HOST backtrace. The last is the valuable one, because the
// recompiled functions are ordinary host functions named after their guest
// addresses, so a host backtrace through them is the guest call chain.
#pragma once

#include <cstdint>
#include <string>

namespace gears
{

// Classifies a faulting address against the guest mapping. Split out from the
// handler because it is the part that decides whether the report says anything
// useful, and the only part that can be tested before the process is broken.
//
// This runtime maps guest physical address zero as a real window, so a guest
// null dereference reads live RAM rather than faulting. An address low in the
// mapping is therefore NOT a null pointer, and must never be described as one.
std::string DescribeFaultAddress(uintptr_t guestBase, uint64_t guestSize,
                                 uintptr_t faultAddress);

// Installs handlers for the fatal memory signals. Chains to whatever was
// installed before -- hle_d3d's watchpoint machinery installs its own SIGSEGV
// handler while a watch is armed, and replacing it silently would disable the
// watchpoints instead of reporting anything.
//
// After reporting, the previous disposition runs, so a core is still dumped and
// the exit status is unchanged. This adds output; it changes nothing else.
// Installed with NO mapping, deliberately: it must be armed before the guest
// address space exists, because a fault during startup is as worth reporting as
// one at frame 1560. An earlier version took the mapping here and was called
// with gears::Memory() before SetMemory() had run -- it dereferenced a
// reference to nothing and killed the process on its first line, and three
// 280-second runs were spent reading conclusions out of a log that contained
// one line. Report first, classify later.
void InstallFaultReporter();

// Teaches the reporter where guest memory ended up, once it exists. Until this
// is called a fault is still reported, just without the guest/host distinction.
void SetFaultReportGuestMapping(void* guestBase, uint64_t guestSize);

// Gives the CALLING thread an alternate signal stack. Every thread needs its
// own: without one, a fault that exhausted the thread's stack leaves the
// handler nowhere to run, and the process dies silently -- the precise failure
// this reporter exists to prevent. Guest threads call this as they start.
void InstallSignalStackForThisThread();

// Reports, once, if something has replaced the SIGSEGV handler since install.
// Called from a per-frame path: a displaced reporter is invisible otherwise,
// and it is the difference between a crash that explains itself and six runs
// that said nothing.
void VerifyFaultReporterStillInstalled();

} // namespace gears
