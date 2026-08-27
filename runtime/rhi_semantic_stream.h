#pragma once

#include <array>
#include <cstdint>
#include <variant>
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

struct RhiSemanticBufferRange
{
    std::uint32_t guestAddress = 0;
    std::uint32_t sizeBytes = 0;
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
    RhiSemanticBufferRange vertexData;
    RhiSemanticBufferRange indexData;
};

struct RhiDrawPacketEvidence
{
    bool present = false;
    std::uint32_t opcode = 0;
    std::uint32_t primitiveType = 0;
    std::uint32_t sourceSelect = 0;
    std::uint32_t elementCount = 0;
    bool transientDataPresent = false;
    RhiSemanticBufferRange vertexData;
    RhiSemanticBufferRange indexData;
};

enum class RhiDrawEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

enum class RhiSemanticBindingKind : std::uint8_t
{
    Texture,
    PixelShader,
    VertexShader,
    IndexBuffer,
    VertexStream,
};

struct RhiSemanticBinding
{
    RhiSemanticBindingKind kind = RhiSemanticBindingKind::Texture;
    std::uint32_t slot = 0;
    std::uint32_t object = 0;
    std::array<std::uint32_t, 6> descriptor{};
    std::uint32_t descriptorDwords = 0;
};

struct RhiBindingStateEvidence
{
    bool present = false;
    std::uint32_t observedObject = 0;
    std::array<std::uint32_t, 6> descriptor{};
    std::uint32_t descriptorDwords = 0;
};

enum class RhiBindingEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

struct RhiSemanticPresent
{
    std::uint64_t frameSequence = 0;
    std::uint32_t frontBuffer = 0;
    std::array<std::uint32_t, 6> fetchDescriptor{};
};

struct RhiPresentPacketEvidence
{
    bool present = false;
    bool framingValid = false;
    std::uint32_t frameSequence = 0;
    std::uint32_t frontBuffer = 0;
    std::array<std::uint32_t, 6> fetchDescriptor{};
};

enum class RhiPresentEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

struct RhiObservedDraw
{
    RhiSemanticDraw draw;
    RhiDrawPacketEvidence packet;
    RhiDrawEvidenceResult evidence = RhiDrawEvidenceResult::Missing;
};

struct RhiObservedBinding
{
    RhiSemanticBinding binding;
    RhiBindingStateEvidence state;
    RhiBindingEvidenceResult evidence = RhiBindingEvidenceResult::Missing;
};

struct RhiObservedPresent
{
    RhiSemanticPresent present;
    RhiPresentPacketEvidence packet;
    RhiPresentEvidenceResult evidence = RhiPresentEvidenceResult::Missing;
};

using RhiSemanticEventPayload =
    std::variant<RhiObservedDraw, RhiObservedBinding, RhiObservedPresent>;

struct RhiSemanticEvent
{
    std::uint64_t sequence = 0;
    RhiSemanticEventPayload payload;
};

struct RhiSemanticFrame
{
    std::uint64_t frameSequence = 0;
    std::vector<RhiSemanticEvent> events;
    std::uint64_t draws = 0;
    std::uint64_t bindings = 0;
    std::uint64_t presents = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t bindingsMatched = 0;
    std::uint64_t bindingsMissing = 0;
    std::uint64_t bindingsMismatched = 0;
    std::uint64_t presentsMatched = 0;
    std::uint64_t presentsMissing = 0;
    std::uint64_t presentsMismatched = 0;
};

[[nodiscard]] bool RhiSemanticObservationEnabled();
[[nodiscard]] RhiDrawEvidenceResult CompareRhiDrawPacket(const RhiSemanticDraw &draw,
                                                         const RhiDrawPacketEvidence &packet);
[[nodiscard]] RhiBindingEvidenceResult CompareRhiBindingState(const RhiSemanticBinding &binding,
                                                              const RhiBindingStateEvidence &state);
[[nodiscard]] RhiPresentEvidenceResult
CompareRhiPresentPacket(const RhiSemanticPresent &present, const RhiPresentPacketEvidence &packet);
void ObserveRhiSemanticDraw(const RhiSemanticDraw &draw, const RhiDrawPacketEvidence &packet);
void ObserveRhiSemanticBinding(const RhiSemanticBinding &binding,
                               const RhiBindingStateEvidence &state);
void ObserveRhiSemanticPresent(const RhiSemanticPresent &present,
                               const RhiPresentPacketEvidence &packet);
[[nodiscard]] RhiSemanticFrame SealRhiSemanticFrame(std::uint64_t frameSequence);
void ReportRhiSemanticFrame(std::uint64_t frameSequence);

} // namespace gears
