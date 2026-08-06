// The interleaved render comparer. See render_ab.cpp for why this is in-process
// and why it refuses rather than reports when the two arms issue different
// draws.
#pragma once

#include <string>

namespace gears
{

// Compares two per-draw signature files written by GEARS_DRAW_TRACE_ALL in the
// same process and logs the first divergent draw. Returns false only when the
// comparison could not be MADE (missing input, mismatched draw streams) --
// "the arms agree" is a result, not a failure, and says what it could not see.
bool ReportAbDivergence(const std::string& aPath, const std::string& bPath,
                        const std::string& knob);

} // namespace gears
