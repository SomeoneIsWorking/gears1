#pragma once

#include <cstdint>

// Where the audio pump's callback time goes.
//
// The pump measures WALL vs CPU time of the title's render callback
// (xaudio_null.cpp); when wall is milliseconds and CPU is microseconds, the
// callback is BLOCKED inside a kernel primitive, and the next question is
// which one. These probes time the blocking primitives, but only on the pump
// thread inside the callback -- everywhere else they are a thread-local test
// and nothing more, so the guest's own hot paths pay nothing measurable.
//
// Accumulation is single-threaded by construction: only the pump thread
// records (the flag is thread-local to it) and only the pump thread reports
// (from its periodic log line). No atomics needed, no contention added to the
// very locks being measured.
namespace gears
{

// True on the audio pump's host thread while it is inside the title's render
// callback. Set by the pump (xaudio_null.cpp); read by the probes and by the
// XMA kick path (a kick on this thread spends the callback's slot budget).
extern thread_local bool t_inAudioPumpCallback;

// Adds `nanos` of blocked time at `site`. `site` must be a string literal;
// slots are keyed by pointer.
void WaitProbeRecord(const char* site, uint64_t nanos);

// Adds a raw count at `site` -- for quantities that are not times (poll
// rounds, for one). Reported alongside the times, labelled as a count.
void WaitProbeCount(const char* site, uint64_t n);

// Logs one line with every site's count and total blocked time since the last
// report, then clears the table. Called from the pump's periodic report, on
// the pump thread.
void WaitProbeReport();

// ---------------------------------------------------------------------------
// The stall detector.
//
// The guest sometimes stops making progress while the process stays alive and
// the renderer keeps drawing (catalog #44). "Stopped" and "slow" look the same
// from outside, and attaching a debugger after the fact has not worked, so the
// runtime reports it itself, with the one thing a backtrace would have given:
// what every guest thread was doing when everything stopped.
//
// The heartbeat is every WaitProbe -- that is, every kernel primitive any guest
// thread enters. A guest that is running calls into the kernel constantly. If
// nothing does so for kStallSeconds, either every thread is blocked (and the
// report names what each is blocked on) or threads are spinning in pure guest
// code (and the report says so, which is itself the diagnosis).
void StartStallDetector();

// Progress on a named subsystem. Watched independently, because they fail
// independently: a run has been seen where the renderer kept drawing at full
// rate while the title's audio pipeline stopped dead, and a single global
// progress signal calls that healthy. `channel` must be a string literal.
void NoteGuestProgress(const char* channel);

// Times one potentially-blocking region. Free (one thread-local load and a
// branch) when not on the pump thread inside the callback.
class WaitProbe
{
public:
    explicit WaitProbe(const char* site);
    ~WaitProbe();

    WaitProbe(const WaitProbe&) = delete;
    WaitProbe& operator=(const WaitProbe&) = delete;

private:
    const char* site_;
    uint64_t began_;
    // Whether this scope published the calling thread's state, so the
    // destructor only clears what it set (probes nest: a wait inside a lock).
    bool published_;
};

} // namespace gears
