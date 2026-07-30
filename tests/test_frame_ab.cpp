// Tests for the in-run A/B, written because comparing separate runs does not work
// on this workload and I nearly shipped a conclusion that assumed it did.
//
// WHAT WENT WRONG. Gating a per-draw diagnostic that measured 3.6 ms of a 39.4 ms
// draw loop should have made frames cheaper. Three runs of the same gameplay walk,
// each averaged over its last 800 frames at an identical 743 draws a frame:
//
//   ungated   39.4 ms (sd 1.7)
//   gated     47.2 ms (sd 5.6)
//   gated     42.7 ms (sd 2.5)
//
// The two runs WITH the work removed were both slower than the one with it in. The
// spread between runs is around 8 ms; the effect under test is 3.6. Nothing about
// the change could be concluded from those numbers in either direction -- and the
// tempting reading, "it made things worse", is exactly as unfounded as "it helped".
//
// So the arms have to be interleaved WITHIN one run, frame by frame, where they
// share the machine's thermal state, the scene, the caches and the allocator. And
// the summary has to say when the difference it is reporting cannot be resolved,
// because a bare delta between two means reads as a result no matter how small it
// is next to the noise. That negative is the whole reason this file exists.

#include <cstdio>

#include "frame_ab.h"

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

using gears::AbSummary;
using gears::AbTest;

// Disabled is the default in a normal run, and it must not silently behave like
// arm B: a caller asking "which arm am I?" when no test is running has to get the
// baseline, or the runtime quietly runs the experimental path in production.
void TestDisabledAlwaysRunsTheBaseline()
{
    AbTest ab(/*enabled=*/false);
    for (int i = 0; i < 8; ++i)
    {
        ab.BeginFrame();
        Check(!ab.Arm(), "a disabled A/B always reports the baseline arm");
        ab.RecordFrame(10.0);
    }
    AbSummary s;
    Check(!ab.Summarise(s),
        "and it has no summary to give -- there was no experiment");
}

// The arms alternate every frame. Blocking them (100 frames of A then 100 of B)
// would let a thermal ramp or a scene change land entirely on one arm, which is
// the failure the separate runs already demonstrated.
void TestArmsAlternateEveryFrame()
{
    AbTest ab(true);
    bool previous = false;
    for (int i = 0; i < 10; ++i)
    {
        ab.BeginFrame();
        if (i > 0)
            Check(ab.Arm() != previous,
                "consecutive frames take opposite arms, so drift over the run"
                " lands on both arms equally rather than on one of them");
        previous = ab.Arm();
        ab.RecordFrame(10.0);
    }
}

// A frame's cost belongs to the arm that frame ran, and the counts must come out
// even over an even number of frames.
void TestCostsAreAttributedToTheirOwnArm()
{
    AbTest ab(true);
    for (int i = 0; i < 100; ++i)
    {
        ab.BeginFrame();
        ab.RecordFrame(ab.Arm() ? 30.0 : 20.0);
    }
    AbSummary s;
    Check(ab.Summarise(s), "100 frames is enough to summarise");
    Check(s.baselineFrames == 50 && s.armFrames == 50,
        "the alternation splits the frames evenly");
    Check(s.baselineMs > 19.9 && s.baselineMs < 20.1,
        "baseline frames average their own cost, not the pooled one");
    Check(s.armMs > 29.9 && s.armMs < 30.1,
        "and so do the experimental ones");
    Check(s.differenceMs > 9.9 && s.differenceMs < 10.1,
        "the difference is arm minus baseline, so positive means SLOWER");
}

// The point of the file. Two arms with means 1 ms apart and a per-frame spread of
// several ms have not been distinguished, and saying "B is 1 ms slower" there is
// the same mistake as reading a 3.6 ms effect out of an 8 ms run-to-run gap.
void TestAnUnresolvableDifferenceSaysSo()
{
    AbTest ab(true);
    // Alternating 10 and 30 within EACH arm: both arms average 20, both have a
    // spread of 10, and any difference between them is noise by construction.
    for (int i = 0; i < 200; ++i)
    {
        ab.BeginFrame();
        ab.RecordFrame((i % 4 < 2) ? 10.0 : 30.0);
    }
    AbSummary s;
    Check(ab.Summarise(s), "there is data to summarise");
    Check(!s.resolved,
        "a difference smaller than the noise is reported as UNRESOLVED rather"
        " than as a small result -- this is the case that made the separate-run"
        " comparison useless");
    Check(s.noiseMs > 0.0,
        "and the noise it was judged against is reported, so the reader can see"
        " how big an effect this run COULD have detected");
}

// The mirror case: a real effect much larger than the spread must be called real,
// or the guard is just a way of never concluding anything.
void TestAClearDifferenceIsResolved()
{
    AbTest ab(true);
    for (int i = 0; i < 200; ++i)
    {
        ab.BeginFrame();
        // Each arm varies by +/-0.5 ms around its own mean; the arms are 10 apart.
        const double jitter = (i % 4 < 2) ? -0.5 : 0.5;
        ab.RecordFrame((ab.Arm() ? 30.0 : 20.0) + jitter);
    }
    AbSummary s;
    Check(ab.Summarise(s), "there is data to summarise");
    Check(s.resolved,
        "a 10 ms difference against a 0.5 ms spread is resolved -- a test that"
        " cannot say yes is no more useful than one that cannot say no");
    Check(s.differenceMs > 9.0,
        "and the difference is reported at roughly its true size");
}

// Too few frames cannot resolve anything, however clean they look. Two frames of
// each arm that happen to differ are not evidence.
void TestTooFewFramesIsNotAResult()
{
    AbTest ab(true);
    for (int i = 0; i < 4; ++i)
    {
        ab.BeginFrame();
        ab.RecordFrame(ab.Arm() ? 30.0 : 20.0);
    }
    AbSummary s;
    Check(!ab.Summarise(s) || !s.resolved,
        "four frames do not resolve a difference no matter how clean they are;"
        " a first-frame render includes shader translation and pipeline creation"
        " that no later frame pays, so early frames are not the steady state at"
        " all");
}

// A summary asked for before any frame has run must refuse rather than return
// zeroes, which would read as "both arms cost nothing and are identical".
void TestNoDataRefusesRatherThanReturningZero()
{
    AbTest ab(true);
    AbSummary s;
    Check(!ab.Summarise(s),
        "with no frames recorded there is no summary -- zeroed means would read"
        " as a measured tie");
}

// The frames a caller wants excluded (warm-up, the first render, a report frame
// that also writes a PPM) are dropped by simply not recording them, and dropping
// one must not desynchronise the alternation.
void TestSkippedFramesDoNotBiasTheArms()
{
    AbTest ab(true);
    int recorded = 0;
    for (int i = 0; i < 100; ++i)
    {
        ab.BeginFrame();
        if (i < 10)
            continue;   // warm-up: the arm still flipped, the cost is not counted
        ab.RecordFrame(ab.Arm() ? 30.0 : 20.0);
        ++recorded;
    }
    AbSummary s;
    Check(ab.Summarise(s), "the recorded frames summarise");
    Check(s.baselineFrames + s.armFrames == recorded,
        "only recorded frames count, so warm-up cannot land in a mean");
    Check(s.baselineMs > 19.9 && s.baselineMs < 20.1 &&
          s.armMs > 29.9 && s.armMs < 30.1,
        "and skipping frames does not smear one arm's cost into the other");
}

} // namespace

int main()
{
    TestDisabledAlwaysRunsTheBaseline();
    TestArmsAlternateEveryFrame();
    TestCostsAreAttributedToTheirOwnArm();
    TestAnUnresolvableDifferenceSaysSo();
    TestAClearDifferenceIsResolved();
    TestTooFewFramesIsNotAResult();
    TestNoDataRefusesRatherThanReturningZero();
    TestSkippedFramesDoNotBiasTheArms();

    if (g_failures == 0)
    {
        printf("all frame A/B tests passed\n");
        return 0;
    }
    printf("%d frame A/B test(s) FAILED\n", g_failures);
    return 1;
}
