// Pacing the audio pump: when the next render-driver callback is due, and what
// to do about the ones that are already late.
//
// The console asks the title for one 256-sample frame every 1/187.5 s, from the
// DAC's own clock. A host cannot always deliver that -- the callback blocks on
// the title's own mixer thread, the scheduler takes the core away -- so the
// interesting question is not "when is the next slot" but WHAT TO DO WHEN SLOTS
// HAVE BEEN MISSED, and that question has two answers with very different sounds:
//
//   CATCH UP: fire one callback for every missed slot, back to back. The title
//   then advances its music by 256 samples per call as fast as the calls arrive,
//   so a second of wall time can consume two seconds of the stream. The pitch
//   goes UP and stays up for as long as the deficit lasts, and the frames pile
//   into the host device faster than 48 kHz can drain them.
//
//   RE-BASE: give up on the missed slots and schedule the next one from now. The
//   title's stream then advances in real time. What was missed is missed -- there
//   is a gap -- but a gap is a glitch and a burst is a permanently wrong pitch.
//
// So this pacer re-bases, and COUNTS what it dropped rather than hiding it: a
// pump that quietly skips is indistinguishable from a pump that is keeping up.
//
// It is a pure function of the two time points so it can be tested without a
// clock, a thread or a guest -- see tests/test_audio_pace.cpp.
#pragma once

#include <cstdint>

namespace gears
{

struct PaceDecision
{
    // When the next callback should happen, as nanoseconds on the same clock the
    // caller passed in.
    int64_t nextSlotNanos = 0;
    // Slots that were due and will never be served. Zero on the healthy path.
    int64_t droppedSlots = 0;
};

// `nowNanos`    -- the clock, after the previous callback returned.
// `dueNanos`    -- when the slot that just ran was due.
// `periodNanos` -- 1/187.5 s in nanoseconds.
// `maxBehindSlots` -- how far behind the pump may fall before it stops trying to
//                     catch up. One slot of slack absorbs ordinary jitter, so a
//                     late-but-recovering pump still delivers every frame; beyond
//                     that, catching up is the thing that ruins the pitch.
inline PaceDecision PaceAudioPump(int64_t nowNanos, int64_t dueNanos,
                                  int64_t periodNanos, int64_t maxBehindSlots = 1)
{
    PaceDecision out;
    const int64_t next = dueNanos + periodNanos;
    if (periodNanos <= 0)
    {
        out.nextSlotNanos = nowNanos;
        return out;
    }
    const int64_t behind = nowNanos - next;
    if (behind <= maxBehindSlots * periodNanos)
    {
        // On time, or close enough that the next slot can still be served in
        // order. This is the only path that keeps every frame.
        out.nextSlotNanos = next;
        return out;
    }
    // Too far behind to serve the missed slots at their own rate. Re-base to the
    // next slot boundary at or after now, and say how many were dropped.
    out.droppedSlots = behind / periodNanos;
    out.nextSlotNanos = next + out.droppedSlots * periodNanos;
    return out;
}

} // namespace gears
