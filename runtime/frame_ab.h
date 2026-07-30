// An A/B test whose two arms are interleaved WITHIN one run, frame by frame.
//
// WHY THIS EXISTS. Comparing two runs of the title does not resolve anything below
// about ten milliseconds a frame. Three runs of the same scripted gameplay walk,
// each averaged over its last 800 rendered frames at an identical 743 draws a
// frame, gave draw loops of 39.4 ms (sd 1.7), 47.2 ms (sd 5.6) and 42.7 ms
// (sd 2.5) -- and the two slower ones were the runs with 3.6 ms of work REMOVED.
// The between-run spread is roughly 8 ms against a within-run spread of 2, so the
// run is the dominant variable and any effect smaller than it is unmeasurable that
// way, in either direction.
//
// Alternating the arms every frame puts both of them in the same run, on the same
// thermal state, the same scene, the same caches and the same allocator, and lets
// drift over the run land equally on both.
//
// The summary is required to say when it CANNOT tell the arms apart. A difference
// of means printed on its own reads as a finding however small it is next to the
// noise -- which is precisely the reading that the three runs above invite and
// that has no support at all.
#pragma once

#include <cstdint>

namespace gears
{

struct AbSummary
{
    double baselineMs = 0.0;    // mean cost of the frames that ran the baseline
    double armMs = 0.0;         // mean cost of the frames that ran the experiment
    double differenceMs = 0.0;  // arm minus baseline: POSITIVE means slower
    double noiseMs = 0.0;       // the smallest difference this run could resolve
    uint64_t baselineFrames = 0;
    uint64_t armFrames = 0;
    bool resolved = false;      // is the difference bigger than the noise?
};

class AbTest
{
public:
    // Disabled is the normal state of a run. A disabled test always reports the
    // BASELINE arm, so a caller that forgets to check cannot end up running the
    // experimental path in production.
    explicit AbTest(bool enabled) : enabled_(enabled) {}

    // Call once per rendered frame, before anything reads Arm().
    void BeginFrame() { if (enabled_) arm_ = !arm_; }

    // Which arm is this frame? False is the baseline.
    bool Arm() const { return enabled_ && arm_; }

    bool Enabled() const { return enabled_; }

    // The cost of the frame that just finished. Frames deliberately left out --
    // warm-up, the first render that pays for every shader translation and
    // pipeline in the frame, a report frame that also writes an image -- are
    // simply not recorded; the arm still alternated, so leaving them out cannot
    // bias one arm.
    void RecordFrame(double ms);

    // False when there is nothing to say: the test is off, or no frames were
    // recorded. It does NOT return zeroed means in that case, because two zeroed
    // means read as a measured tie.
    bool Summarise(AbSummary& out) const;

private:
    bool enabled_ = false;
    bool arm_ = true;   // BeginFrame flips first, so frame 1 is the baseline
    uint64_t n_[2] = {0, 0};
    double sum_[2] = {0.0, 0.0};
    double sumSq_[2] = {0.0, 0.0};
};

} // namespace gears
