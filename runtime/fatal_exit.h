#pragma once

#include <cstdio>
#include <cstdlib>

namespace gears
{

// Ending the process from a guest thread that has decided it cannot continue.
//
// It must NOT be std::exit. std::exit runs the atexit handlers and destroys
// every function-local static, and here it is being called from one of about
// twenty guest threads while the other nineteen are still running: they keep
// calling into the runtime, and the statics they call through -- the critical
// section table in kernel_sync.cpp, for one -- have just been freed underneath
// them. AddressSanitizer caught precisely that (a heap-use-after-free in
// HostLockFor on the audio pump thread, freed by ~unordered_map running from
// __run_exit_handlers, from KeBugCheck on another thread), and it is the source
// of the second, misleading crash that then buried the first one. std::exit
// from a live multithreaded process is undefined behaviour, not a race we could
// close by ordering the teardown differently.
//
// _Exit is the C and C++ standards' answer: terminate now, run no handlers,
// destroy no statics. Nothing the runtime registers needs to run: the two
// atexit handlers are the audio device close, which the operating system
// reclaims anyway, and the WAV dump's Close -- and that writer already
// refreshes its header about once a second precisely because runs "end by being
// killed far more often than they end cleanly" (xaudio_null.cpp), so it survives
// this the same way it survives SIGKILL. The explicit flush covers a buffered
// log sink, since _Exit is not required to flush.
[[noreturn]] inline void FatalExit(int code)
{
    std::fflush(nullptr);
    std::_Exit(code);
}

} // namespace gears
