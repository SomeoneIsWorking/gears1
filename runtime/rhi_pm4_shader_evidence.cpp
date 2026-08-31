#include "rhi_pm4_shader_evidence.h"

#include "guest_address.h"
#include "gpu_draw.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <utility>

#include <lucent/log.h>

namespace gears
{
namespace
{

struct SemanticFrame
{
    bool valid = true;
    RhiSemanticFrame frame;
};

struct Pm4Frame
{
    bool valid = true;
    std::vector<RhiPm4DrawShaderEvidence> draws;
};

struct JoinState
{
    std::mutex mutex;
    std::map<std::uint64_t, SemanticFrame> semanticFrames;
    std::map<std::uint64_t, Pm4Frame> pm4Frames;
    std::map<std::uint64_t, RhiPm4ShaderFrameComparison> completedFrames;
    std::uint64_t retiredCompletedThrough = 0;
    std::uint64_t highestSequence = 0;
};

JoinState g_join;
constexpr std::size_t kPm4JoinHistory = 64;
std::mutex g_reportMutex;
bool g_reportedFailure = false;

[[nodiscard]] std::uint32_t CanonicalPacketAddress(std::uint32_t address)
{
    return address & kGuestPhysicalAddressMask & ~std::uint32_t{3};
}

[[nodiscard]] bool ContainsModule(const RhiSemanticBinding &binding, std::uint64_t hash)
{
    return std::ranges::any_of(binding.shaderModules, [hash](const RhiShaderModuleEvidence &module)
                               { return module.hash == hash; });
}

enum class ModuleMatch : std::uint8_t
{
    Match,
    Missing,
    Mismatch,
};

[[nodiscard]] ModuleMatch CompareModule(const std::optional<RhiSemanticBinding> &binding,
                                        std::uint64_t executedHash)
{
    if (!binding.has_value() || binding->shaderModules.empty() || executedHash == 0)
        return ModuleMatch::Missing;
    return ContainsModule(*binding, executedHash) ? ModuleMatch::Match : ModuleMatch::Mismatch;
}

[[nodiscard]] RhiPm4ShaderFrameComparison
CompareFrame(const RhiSemanticFrame &semantic, const std::vector<RhiPm4DrawShaderEvidence> &pm4)
{
    RhiPm4ShaderFrameComparison result{.executedDraws = pm4.size()};
    std::map<std::uint32_t, std::vector<const RhiPm4DrawShaderEvidence *>> executions;
    for (const RhiPm4DrawShaderEvidence &draw : pm4)
    {
        const std::uint32_t packet = CanonicalPacketAddress(draw.packetGuestAddress);
        if (packet == 0)
        {
            ++result.unkeyedExecutedDraws;
            continue;
        }
        executions[packet].push_back(&draw);
    }

    std::map<std::uint32_t, const RhiObservedDraw *> semanticDraws;
    for (const RhiSemanticEvent &event : semantic.events)
    {
        const auto *draw = std::get_if<RhiObservedDraw>(&event.payload);
        if (draw == nullptr)
            continue;
        ++result.semanticDraws;
        const std::uint32_t packet = CanonicalPacketAddress(draw->packet.packetGuestAddress);
        if (packet == 0)
        {
            ++result.unkeyedSemanticDraws;
            ++result.missing;
            if (result.firstMissingSemanticPacket == 0)
                result.firstMissingSemanticPacket = packet;
            continue;
        }
        const auto [it, inserted] = semanticDraws.emplace(packet, draw);
        if (!inserted)
        {
            ++result.mismatched;
            if (result.firstMismatchedSemanticPacket == 0)
                result.firstMismatchedSemanticPacket = packet;
        }
    }

    for (const auto &[packet, draw] : semanticDraws)
    {
        const auto executionsIt = executions.find(packet);
        if (executionsIt == executions.end())
        {
            ++result.missing;
            if (result.firstMissingSemanticPacket == 0)
                result.firstMissingSemanticPacket = packet;
            continue;
        }

        const RhiPm4DrawShaderEvidence &first = *executionsIt->second.front();
        const bool consistent =
            std::ranges::all_of(executionsIt->second,
                                [&first](const RhiPm4DrawShaderEvidence *execution)
                                {
                                    return execution->vertexShaderHash == first.vertexShaderHash &&
                                           execution->pixelShaderHash == first.pixelShaderHash;
                                });
        RhiSemanticDrawState state = draw->state;
        ApplyRhiShaderPacketModuleEvidence(state, packet);
        const ModuleMatch vertex = CompareModule(state.vertexShader, first.vertexShaderHash);
        const ModuleMatch pixel = CompareModule(state.pixelShader, first.pixelShaderHash);
        if (!consistent || vertex == ModuleMatch::Mismatch || pixel == ModuleMatch::Mismatch)
        {
            ++result.mismatched;
            if (result.firstMismatchedSemanticPacket == 0)
                result.firstMismatchedSemanticPacket = packet;
        }
        else if (vertex == ModuleMatch::Missing || pixel == ModuleMatch::Missing)
        {
            ++result.missing;
            if (result.firstMissingSemanticPacket == 0)
                result.firstMissingSemanticPacket = packet;
        }
        else
        {
            ++result.matched;
        }
        executions.erase(executionsIt);
    }
    result.unmatchedExecutedPackets = executions.size();
    return result;
}

void RememberCompleted(std::uint64_t sequence, const RhiPm4ShaderFrameComparison &comparison)
{
    g_join.completedFrames.insert_or_assign(sequence, comparison);
    while (g_join.completedFrames.size() > kPm4JoinHistory)
    {
        const auto completed = g_join.completedFrames.begin();
        g_join.retiredCompletedThrough = std::max(g_join.retiredCompletedThrough, completed->first);
        g_join.completedFrames.erase(completed);
    }
}

[[nodiscard]] std::optional<RhiPm4ShaderFrameComparison>
TakeComparisonLocked(std::uint64_t sequence)
{
    const auto semantic = g_join.semanticFrames.find(sequence);
    const auto pm4 = g_join.pm4Frames.find(sequence);
    if (semantic == g_join.semanticFrames.end() || pm4 == g_join.pm4Frames.end())
        return std::nullopt;

    RhiPm4ShaderFrameComparison comparison =
        CompareFrame(semantic->second.frame, pm4->second.draws);
    comparison.duplicate = !semantic->second.valid || !pm4->second.valid;
    g_join.semanticFrames.erase(semantic);
    g_join.pm4Frames.erase(pm4);
    RememberCompleted(sequence, comparison);
    return comparison;
}

void RetireStaleJoinsLocked(std::uint64_t newestSequence)
{
    g_join.highestSequence = std::max(g_join.highestSequence, newestSequence);
    if (g_join.highestSequence <= kPm4JoinHistory)
        return;
    const std::uint64_t oldestAllowed = g_join.highestSequence - kPm4JoinHistory;
    while (!g_join.semanticFrames.empty() && g_join.semanticFrames.begin()->first < oldestAllowed)
    {
        const std::uint64_t sequence = g_join.semanticFrames.begin()->first;
        lucent::error("rhi", "PM4 shader evidence never arrived for semantic frame {}", sequence);
        g_join.semanticFrames.erase(g_join.semanticFrames.begin());
        g_join.retiredCompletedThrough = std::max(g_join.retiredCompletedThrough, sequence);
    }
    while (!g_join.pm4Frames.empty() && g_join.pm4Frames.begin()->first < oldestAllowed)
    {
        const std::uint64_t sequence = g_join.pm4Frames.begin()->first;
        lucent::error("rhi", "semantic frame never arrived for PM4 shader evidence {}", sequence);
        g_join.pm4Frames.erase(g_join.pm4Frames.begin());
        g_join.retiredCompletedThrough = std::max(g_join.retiredCompletedThrough, sequence);
    }
}

void ReportComparison(std::uint64_t sequence, const RhiPm4ShaderFrameComparison &comparison)
{
    const bool failed =
        comparison.missing != 0 || comparison.mismatched != 0 || comparison.duplicate;
    bool firstFailure = false;
    {
        std::lock_guard guard(g_reportMutex);
        firstFailure = failed && !g_reportedFailure;
        g_reportedFailure = g_reportedFailure || failed;
    }
    if (sequence != 1 && sequence % 60 != 0 && !firstFailure)
        return;
    lucent::info("rhi",
                 "PM4 shader evidence through frame {}: {} semantic draw(s), {} executed draw(s), "
                 "{} match(es), {} missing, {} mismatch(es); {} unkeyed semantic, {} unkeyed "
                 "executed, {} unmatched PM4 packet(s), duplicate={}",
                 sequence, comparison.semanticDraws, comparison.executedDraws, comparison.matched,
                 comparison.missing, comparison.mismatched, comparison.unkeyedSemanticDraws,
                 comparison.unkeyedExecutedDraws, comparison.unmatchedExecutedPackets,
                 comparison.duplicate);
}

template <typename Frame, typename Frames>
void InsertFrame(std::uint64_t sequence, Frame frame, Frames &frames)
{
    const auto [it, inserted] = frames.emplace(sequence, std::move(frame));
    if (!inserted)
        it->second.valid = false;
}

} // namespace

std::optional<RhiPm4ShaderFrameComparison>
PublishRhiPm4FrameShaderEvidence(std::uint64_t frameSequence,
                                 std::vector<RhiPm4DrawShaderEvidence> draws)
{
    if (!RhiSemanticObservationEnabled())
        return std::nullopt;

    std::optional<RhiPm4ShaderFrameComparison> comparison;
    {
        std::lock_guard guard(g_join.mutex);
        if (frameSequence <= g_join.retiredCompletedThrough ||
            g_join.completedFrames.contains(frameSequence))
        {
            comparison = RhiPm4ShaderFrameComparison{.duplicate = true};
        }
        else
        {
            InsertFrame(frameSequence, Pm4Frame{.draws = std::move(draws)}, g_join.pm4Frames);
            comparison = TakeComparisonLocked(frameSequence);
            RetireStaleJoinsLocked(frameSequence);
        }
    }
    if (comparison.has_value())
        ReportComparison(frameSequence, *comparison);
    return comparison;
}

void ObserveRhiPm4FrameShaderEvidence(std::uint64_t frameSequence,
                                      const std::vector<FrameDrawItem> &draws)
{
    if (!RhiSemanticObservationEnabled())
        return;

    std::vector<RhiPm4DrawShaderEvidence> evidence;
    evidence.reserve(draws.size());
    for (const FrameDrawItem &draw : draws)
    {
        evidence.push_back({.packetGuestAddress = draw.packetGuestAddress,
                            .packetBufferBase = draw.packetBufferBase,
                            .packetFromIndirectBuffer = draw.packetFromIndirectBuffer,
                            .vertexShaderHash = draw.vsHash,
                            .pixelShaderHash = draw.psHash});
    }
    (void)PublishRhiPm4FrameShaderEvidence(frameSequence, std::move(evidence));
}

std::optional<RhiPm4ShaderFrameComparison>
ObserveRhiSemanticFrameForPm4ShaderEvidence(const RhiSemanticFrame &frame)
{
    if (!RhiSemanticObservationEnabled())
        return std::nullopt;

    std::optional<RhiPm4ShaderFrameComparison> comparison;
    {
        std::lock_guard guard(g_join.mutex);
        if (frame.frameSequence <= g_join.retiredCompletedThrough ||
            g_join.completedFrames.contains(frame.frameSequence))
        {
            comparison = RhiPm4ShaderFrameComparison{.duplicate = true};
        }
        else
        {
            InsertFrame(frame.frameSequence, SemanticFrame{.frame = frame}, g_join.semanticFrames);
            comparison = TakeComparisonLocked(frame.frameSequence);
            RetireStaleJoinsLocked(frame.frameSequence);
        }
    }
    if (comparison.has_value())
        ReportComparison(frame.frameSequence, *comparison);
    return comparison;
}

} // namespace gears
