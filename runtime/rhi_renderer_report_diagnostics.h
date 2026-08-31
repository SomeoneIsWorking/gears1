#pragma once

#include <cstdint>
#include <string>

namespace gears
{

struct RhiRendererFrameComparison;
enum class RhiRendererDrawEvidenceReason : std::uint8_t;

// The terminal renderer report owns diagnostic aggregation separately from
// semantic-to-renderer joining, so evidence growth cannot turn the join owner
// into a reporting monolith.
void ReportRhiRendererEvidenceCensus(const RhiRendererFrameComparison &comparison);
[[nodiscard]] const char *RhiRendererEvidenceReasonName(RhiRendererDrawEvidenceReason reason);
[[nodiscard]] std::string
DescribeRhiRendererUnmatchedPacket(const RhiRendererFrameComparison &comparison);

} // namespace gears
