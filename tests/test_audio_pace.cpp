// Tests for the audio pump's pacing policy.
//
// The property under test is the one that decides what the game SOUNDS like when
// the host cannot keep up: a pump that serves missed slots back to back makes the
// title consume its audio stream faster than real time, which is heard as a
// permanently raised pitch, plus frames arriving at the device faster than 48 kHz
// drains them. Dropping the missed slots keeps the stream in real time and costs
// a gap instead.
//
// These are all pure arithmetic on nanosecond counts -- no clock, no thread, no
// guest -- which is the point: the policy is testable, and the thread that uses
// it is not.

#include <cstdio>
#include <cstdint>

#include "audio_pace.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

// 48000 / 256 = 187.5 Hz.
constexpr int64_t kPeriod = 5'333'333;

using gears::PaceAudioPump;

void TestOnTimeKeepsTheCadence()
{
    // The callback returned well inside its slot: the next slot is exactly one
    // period after the one that just ran, and nothing is dropped. Anything else
    // here would make a HEALTHY pump drift.
    const auto d = PaceAudioPump(/*now=*/1'000'000, /*due=*/0, kPeriod);
    Check(d.nextSlotNanos == kPeriod, "an on-time slot schedules the next one a period later");
    Check(d.droppedSlots == 0, "and drops nothing");
}

void TestSmallOverrunStillServesEverySlot()
{
    // The callback overran its slot by a little. This must NOT drop a frame:
    // ordinary jitter has to be absorbed, or a pump that is merely late starts
    // punching holes in the audio.
    const auto d = PaceAudioPump(/*now=*/kPeriod + kPeriod / 2, /*due=*/0, kPeriod);
    Check(d.droppedSlots == 0, "a slot-and-a-half overrun drops nothing");
    Check(d.nextSlotNanos == kPeriod, "and the next slot is still the next one in order");
}

// THE POINT OF THE FILE.
void TestALongStallDoesNotBurst()
{
    // The pump was away for 20 slots -- the measured case is a blocked callback
    // or a descheduled thread. Serving those 20 slots back to back is what makes
    // the title race through its stream; the pacer must give them up and
    // re-base, and it must SAY how many it gave up.
    const int64_t now = 20 * kPeriod;
    const auto d = PaceAudioPump(now, /*due=*/0, kPeriod);
    Check(d.droppedSlots == 19, "a 20-slot stall reports the missed slots");
    Check(d.nextSlotNanos >= now,
        "and schedules the next callback in the FUTURE -- a slot in the past would"
        " fire immediately, which is the burst this exists to prevent");
    Check(d.nextSlotNanos < now + kPeriod,
        "but no further away than one period, or the gap grows for no reason");
}

void TestRepeatedStallsDoNotAccumulateADeficit()
{
    // Feed the pacer a run of stalls and check the schedule never falls behind
    // the clock. A pacer that returns a past deadline once is a pacer that fires
    // a burst; a pacer that does it repeatedly never recovers, which is exactly
    // the "backlog 12133 slots" the runtime reported.
    int64_t due = 0;
    int64_t now = 0;
    int64_t dropped = 0;
    for (int i = 0; i < 200; ++i)
    {
        now += (i % 10 == 0) ? 8 * kPeriod : kPeriod; // every tenth slot stalls
        const auto d = PaceAudioPump(now, due, kPeriod);
        Check(d.nextSlotNanos + kPeriod > now,
            "the schedule never drifts more than a slot into the past");
        dropped += d.droppedSlots;
        due = d.nextSlotNanos;
    }
    Check(dropped > 0, "and a run with real stalls reports dropped slots rather than"
        " silently absorbing them");
}

void TestDegenerateperiodIsNotAnInfiniteLoop()
{
    const auto d = PaceAudioPump(/*now=*/5, /*due=*/0, /*period=*/0);
    Check(d.nextSlotNanos == 5, "a zero period schedules now rather than dividing by zero");
    Check(d.droppedSlots == 0, "and reports nothing dropped");
}

} // namespace

int main()
{
    TestOnTimeKeepsTheCadence();
    TestSmallOverrunStillServesEverySlot();
    TestALongStallDoesNotBurst();
    TestRepeatedStallsDoNotAccumulateADeficit();
    TestDegenerateperiodIsNotAnInfiniteLoop();
    if (g_failures != 0)
    {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("audio pacing: all checks passed\n");
    return 0;
}
