#pragma once

#include "gpu_draw_native_input.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gears
{

struct RhiSemanticDrawState;
struct RhiSemanticFrame;
struct FrameDrawInputs;
struct FrameDrawItem;

// A terminal projection of one source draw after the compatibility renderer
// either materialized NativeDrawInput or explicitly refused it. Source ordinals
// preserve gaps; a refused draw cannot shift every later comparison.
struct RhiRendererDrawInput
{
    std::size_t sourceOrdinal = 0;
    std::uint32_t packetGuestAddress = 0;
    draw::NativeDrawMaterializationOutcome outcome =
        draw::NativeDrawMaterializationOutcome::Refused;
    std::uint32_t primitiveType = 0;
    std::uint32_t elementCount = 0;
    bool indexed = false;
    bool indexIs32 = false;
    std::uint32_t indexEndian = 0;
    std::uint32_t indexGuestBase = 0;
};

struct RhiRendererFrameInput
{
    draw::NativeFrameMaterializationStatus status =
        draw::NativeFrameMaterializationStatus::Complete;
    std::vector<RhiRendererDrawInput> draws;
};

enum class RhiRendererDrawEvidenceResult : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

struct RhiRendererFrameComparison
{
    draw::NativeFrameMaterializationStatus status =
        draw::NativeFrameMaterializationStatus::Complete;
    std::uint64_t semanticDraws = 0;
    std::uint64_t sourceDraws = 0;
    std::uint64_t materializedDraws = 0;
    std::uint64_t refusedDraws = 0;
    std::uint64_t resolveDraws = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t unmatchedRendererPackets = 0;
    std::uint64_t unkeyedRendererDraws = 0;
    std::uint32_t firstMissingSemanticPacket = 0;
    std::uint32_t firstUnmatchedRendererPacket = 0;
    bool duplicate = false;
};

[[nodiscard]] RhiRendererDrawEvidenceResult
CompareRhiRendererDrawInput(const RhiSemanticDrawState &state,
                            const RhiRendererDrawInput &renderer);
[[nodiscard]] RhiRendererFrameComparison
CompareRhiRendererDraws(const RhiSemanticFrame &frame, const RhiRendererFrameInput &renderer);
void ObserveRhiRendererMaterialization(std::uint64_t frameSequence,
                                       const draw::NativeFrameMaterialization &materialization);
void ObserveRhiRendererFrameDropped(std::uint64_t frameSequence);
void SetRhiPacketIdentity(FrameDrawItem &draw, std::uint32_t sourceBase, std::uint32_t ringBase,
                          std::uint32_t sourceIndex);
[[nodiscard]] std::optional<RhiRendererFrameComparison>
PublishRhiRendererFrameInput(std::uint64_t frameSequence, RhiRendererFrameInput frame);

// Completes the asynchronous join when the title-semantic side arrives after
// or before renderer materialization. This is an internal production boundary,
// separate from semantic event capture and state tracking.
[[nodiscard]] std::optional<RhiRendererFrameComparison>
ObserveRhiSemanticFrameSealed(const RhiSemanticFrame &frame);

// Makes every command-processor frame attempt terminal: early exits publish a
// dropped result, while Attach transfers publication to the renderer callback.
class RhiRendererMaterializationGuard
{
  public:
    explicit RhiRendererMaterializationGuard(std::uint64_t frameSequence);
    ~RhiRendererMaterializationGuard();

    RhiRendererMaterializationGuard(const RhiRendererMaterializationGuard &) = delete;
    RhiRendererMaterializationGuard &operator=(const RhiRendererMaterializationGuard &) = delete;

    void Attach(FrameDrawInputs &inputs);

  private:
    std::uint64_t frameSequence_ = 0;
    bool pending_ = false;
};

} // namespace gears
