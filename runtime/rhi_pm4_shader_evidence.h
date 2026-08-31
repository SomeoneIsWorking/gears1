#pragma once

#include "rhi_semantic_stream.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace gears
{

struct FrameDrawItem;

// Exact shader selections from the command processor's sequencer state at a
// DRAW_INDX packet. This is execution evidence, not a semantic binding: it
// cannot repair or replace title-adapter state.
struct RhiPm4DrawShaderEvidence
{
    std::uint32_t packetGuestAddress = 0;
    std::uint32_t packetBufferBase = 0;
    bool packetFromIndirectBuffer = false;
    std::uint64_t vertexShaderHash = 0;
    std::uint64_t pixelShaderHash = 0;
};

struct RhiPm4ShaderFrameComparison
{
    std::uint64_t semanticDraws = 0;
    std::uint64_t executedDraws = 0;
    std::uint64_t matched = 0;
    std::uint64_t missing = 0;
    std::uint64_t mismatched = 0;
    std::uint64_t unkeyedSemanticDraws = 0;
    std::uint64_t unkeyedExecutedDraws = 0;
    std::uint64_t unmatchedExecutedPackets = 0;
    std::uint32_t firstMissingSemanticPacket = 0;
    std::uint32_t firstMismatchedSemanticPacket = 0;
    bool duplicate = false;
};

// The command processor calls this once for every accepted swap packet, after
// it has captured all preceding draw packets. Semantic frame sealing occurs at
// guest VdSwap, so this deliberately completes an asynchronous two-sided join.
[[nodiscard]] std::optional<RhiPm4ShaderFrameComparison>
PublishRhiPm4FrameShaderEvidence(std::uint64_t frameSequence,
                                 std::vector<RhiPm4DrawShaderEvidence> draws);
void ObserveRhiPm4FrameShaderEvidence(std::uint64_t frameSequence,
                                      const std::vector<FrameDrawItem> &draws);

// Called by the semantic stream when guest VdSwap seals a frame. The PM4 side
// may arrive later, when that swap packet executes on the command processor.
[[nodiscard]] std::optional<RhiPm4ShaderFrameComparison>
ObserveRhiSemanticFrameForPm4ShaderEvidence(const RhiSemanticFrame &frame);

} // namespace gears
