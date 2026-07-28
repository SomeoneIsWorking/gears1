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
void PumpWaitRecord(const char* site, uint64_t nanos);

// Adds a raw count at `site` -- for quantities that are not times (poll
// rounds, for one). Reported alongside the times, labelled as a count.
void PumpWaitCount(const char* site, uint64_t n);

// Logs one line with every site's count and total blocked time since the last
// report, then clears the table. Called from the pump's periodic report, on
// the pump thread.
void PumpWaitReport();

// Times one potentially-blocking region. Free (one thread-local load and a
// branch) when not on the pump thread inside the callback.
class PumpWaitScope
{
public:
    explicit PumpWaitScope(const char* site);
    ~PumpWaitScope();

    PumpWaitScope(const PumpWaitScope&) = delete;
    PumpWaitScope& operator=(const PumpWaitScope&) = delete;

private:
    const char* site_;
    uint64_t began_;
};

} // namespace gears
