#include "frame_ab.h"

#include <cmath>

namespace gears
{
namespace
{

// Below this, a difference between the arms is not a difference. Frames early in
// a run are not the steady state at all -- the first render of a scene pays for
// every shader translation and every pipeline in it, and those costs appear in no
// later frame -- so a handful of frames can look arbitrarily clean and mean
// nothing.
constexpr uint64_t kMinimumFramesPerArm = 30;

// How many standard errors the difference has to clear to count as resolved. Two
// is the usual "about 95% of the time this is not chance" line; the exact number
// matters far less than that the comparison happens AT ALL, since the failure this
// class exists to prevent is a bare difference of means printed as a result.
constexpr double kResolvedSigmas = 2.0;

} // namespace

void AbTest::RecordFrame(double ms)
{
    const int index = Arm() ? 1 : 0;
    ++n_[index];
    sum_[index] += ms;
    sumSq_[index] += ms * ms;
}

bool AbTest::Summarise(AbSummary& out) const
{
    if (!enabled_ || (n_[0] == 0 && n_[1] == 0))
        return false;

    out.baselineFrames = n_[0];
    out.armFrames = n_[1];
    out.baselineMs = n_[0] ? sum_[0] / double(n_[0]) : 0.0;
    out.armMs = n_[1] ? sum_[1] / double(n_[1]) : 0.0;
    out.differenceMs = out.armMs - out.baselineMs;

    // The standard error of the difference of two means, from each arm's own
    // spread. Using the arms' own variance rather than a fixed threshold is what
    // makes the answer honest on a noisy run and on a quiet one alike: the same
    // 1 ms difference is a result when frames vary by 0.1 ms and is nothing when
    // they vary by 5.
    const auto variance = [&](int i) {
        if (n_[i] < 2)
            return 0.0;
        const double mean = sum_[i] / double(n_[i]);
        const double v = sumSq_[i] / double(n_[i]) - mean * mean;
        return v > 0.0 ? v * double(n_[i]) / double(n_[i] - 1) : 0.0;
    };
    const double standardError =
        (n_[0] >= 2 && n_[1] >= 2)
            ? std::sqrt(variance(0) / double(n_[0]) + variance(1) / double(n_[1]))
            : 0.0;
    out.noiseMs = kResolvedSigmas * standardError;

    // Two ways to fail to resolve, and both are reported the same way: too few
    // frames, or a difference inside the noise. A run that is simply too short is
    // not a tie, and neither is a run whose arms overlap.
    const bool enoughFrames =
        n_[0] >= kMinimumFramesPerArm && n_[1] >= kMinimumFramesPerArm;
    out.resolved = enoughFrames && standardError > 0.0 &&
                   std::fabs(out.differenceMs) > out.noiseMs;
    return true;
}

} // namespace gears
