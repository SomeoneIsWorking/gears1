#pragma once

#include <cstdint>
#include <vector>

namespace gears
{

enum class RhiSemanticDrawKind : std::uint8_t
{
    TransientVertices,
    TransientVerticesAndIndices,
    BoundVertices,
    BoundIndices,
};

struct RhiSemanticDraw
{
    RhiSemanticDrawKind kind = RhiSemanticDrawKind::BoundVertices;
    std::uint32_t primitiveType = 0;
    std::uint32_t elementCount = 0;
    std::uint32_t baseVertex = 0;
    std::uint32_t startIndex = 0;
    std::uint32_t vertexStrideBytes = 0;
    std::uint32_t indexFormatFlags = 0;
};

struct RhiDrawPacketEvidence
{
    bool present = false;
    std::uint32_t opcode = 0;
    std::uint32_t primitiveType = 0;
    std::uint32_t sourceSelect = 0;
    std::uint32_t elementCount = 0;
};

enum class RhiDrawEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

struct RhiObservedDraw
{
    std::uint64_t sequence = 0;
    RhiSemanticDraw draw;
    RhiDrawPacketEvidence packet;
    RhiDrawEvidenceResult evidence = RhiDrawEvidenceResult::Missing;
};

struct RhiSemanticFrame
{
    std::uint64_t frameSequence = 0;
    std::vector<RhiObservedDraw> draws;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
};

[[nodiscard]] bool RhiSemanticObservationEnabled();
[[nodiscard]] RhiDrawEvidenceResult CompareRhiDrawPacket(const RhiSemanticDraw &draw,
                                                         const RhiDrawPacketEvidence &packet);
void ObserveRhiSemanticDraw(const RhiSemanticDraw &draw, const RhiDrawPacketEvidence &packet);
[[nodiscard]] RhiSemanticFrame SealRhiSemanticFrame(std::uint64_t frameSequence);
void ReportRhiSemanticFrame(std::uint64_t frameSequence);

} // namespace gears
