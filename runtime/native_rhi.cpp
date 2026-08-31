#include "native_rhi.h"

#include <atomic>
#include <cstddef>

#include <lucent/config.h>
#include <lucent/log.h>

namespace gears::native_rhi
{
namespace
{

[[nodiscard]] RhiResourceIdentityEvidence
IdentityFromLifetime(const RhiSemanticResourceLifetime &lifetime)
{
    return {.present = lifetime.object != 0,
            .object = lifetime.object,
            .rawFlags = lifetime.rawFlags,
            .resourceType = lifetime.resourceType,
            .backingObject = lifetime.backingObject,
            .referenceCount = lifetime.previousReferenceCount};
}

[[nodiscard]] bool IsEvidenceMissing(RhiDrawEvidenceResult evidence)
{
    return evidence == RhiDrawEvidenceResult::Missing;
}

[[nodiscard]] bool IsEvidenceMismatch(RhiDrawEvidenceResult evidence)
{
    return evidence == RhiDrawEvidenceResult::Mismatch;
}

[[nodiscard]] bool IsEvidenceMissing(RhiBindingEvidenceResult evidence)
{
    return evidence == RhiBindingEvidenceResult::Missing;
}

[[nodiscard]] bool IsEvidenceMismatch(RhiBindingEvidenceResult evidence)
{
    return evidence == RhiBindingEvidenceResult::Mismatch;
}

[[nodiscard]] bool IsEvidenceMissing(RhiResourceLifetimeEvidenceResult evidence)
{
    return evidence == RhiResourceLifetimeEvidenceResult::Missing;
}

[[nodiscard]] bool IsEvidenceMismatch(RhiResourceLifetimeEvidenceResult evidence)
{
    return evidence == RhiResourceLifetimeEvidenceResult::Mismatch;
}

[[nodiscard]] bool IsEvidenceMissing(RhiVertexStreamResetEvidenceResult evidence)
{
    return evidence == RhiVertexStreamResetEvidenceResult::Missing;
}

[[nodiscard]] bool IsEvidenceMismatch(RhiVertexStreamResetEvidenceResult evidence)
{
    return evidence == RhiVertexStreamResetEvidenceResult::Mismatch;
}

[[nodiscard]] bool IsEvidenceMissing(RhiColorWriteStateEvidenceResult evidence)
{
    return evidence == RhiColorWriteStateEvidenceResult::Missing;
}

[[nodiscard]] bool IsEvidenceMismatch(RhiColorWriteStateEvidenceResult evidence)
{
    return evidence == RhiColorWriteStateEvidenceResult::Mismatch;
}

[[nodiscard]] bool IsEvidenceMissing(RhiResolveEvidenceResult evidence)
{
    return evidence == RhiResolveEvidenceResult::Missing;
}

[[nodiscard]] bool IsEvidenceMismatch(RhiResolveEvidenceResult evidence)
{
    return evidence == RhiResolveEvidenceResult::Mismatch;
}

[[nodiscard]] bool IsEvidenceMissing(RhiPresentEvidenceResult evidence)
{
    return evidence == RhiPresentEvidenceResult::Missing;
}

[[nodiscard]] bool IsEvidenceMismatch(RhiPresentEvidenceResult evidence)
{
    return evidence == RhiPresentEvidenceResult::Mismatch;
}

template <typename Evidence> [[nodiscard]] BuildStatus EvidenceStatus(Evidence evidence)
{
    if (IsEvidenceMissing(evidence))
        return BuildStatus::EvidenceMissing;
    if (IsEvidenceMismatch(evidence))
        return BuildStatus::EvidenceMismatch;
    return BuildStatus::Accepted;
}

} // namespace

BuildResult BuildFrame(const RhiSemanticFrame &observed)
{
    BuildResult result;
    result.frame.frameSequence = observed.frameSequence;
    if (observed.events.empty())
    {
        result.status = BuildStatus::Empty;
        return result;
    }

    result.frame.commands.reserve(observed.events.size());
    std::uint64_t previousSequence = 0;
    bool havePreviousSequence = false;
    bool havePresent = false;

    for (std::size_t index = 0; index < observed.events.size(); ++index)
    {
        const RhiSemanticEvent &event = observed.events[index];
        result.eventIndex = index;
        if (havePreviousSequence && event.sequence <= previousSequence)
        {
            result.status = BuildStatus::SequenceNotIncreasing;
            return result;
        }
        previousSequence = event.sequence;
        havePreviousSequence = true;

        if (const auto *draw = std::get_if<RhiObservedDraw>(&event.payload))
        {
            const BuildStatus status = EvidenceStatus(draw->evidence);
            if (status != BuildStatus::Accepted)
            {
                result.status = status;
                return result;
            }
            result.frame.commands.push_back(
                {.sequence = event.sequence, .payload = DrawCommand{.state = draw->state}});
            ++result.frame.draws;
            continue;
        }
        if (const auto *binding = std::get_if<RhiObservedBinding>(&event.payload))
        {
            const BuildStatus status = EvidenceStatus(binding->evidence);
            if (status != BuildStatus::Accepted)
            {
                result.status = status;
                return result;
            }
            result.frame.commands.push_back(
                {.sequence = event.sequence,
                 .payload = BindingCommand{.binding = binding->binding,
                                           .identity = binding->state.identity}});
            ++result.frame.bindings;
            continue;
        }
        if (const auto *lifetime = std::get_if<RhiObservedResourceLifetime>(&event.payload))
        {
            const BuildStatus status = EvidenceStatus(lifetime->evidence);
            if (status != BuildStatus::Accepted)
            {
                result.status = status;
                return result;
            }
            result.frame.commands.push_back(
                {.sequence = event.sequence,
                 .payload = ResourceLifetimeCommand{.lifetime = lifetime->lifetime,
                                                    .identity =
                                                        IdentityFromLifetime(lifetime->lifetime)}});
            ++result.frame.resourceLifetimeCalls;
            continue;
        }
        if (const auto *construction = std::get_if<RhiObservedResourceConstruction>(&event.payload))
        {
            if (!construction->retained.present)
            {
                result.status = BuildStatus::ConstructionEvidenceMissing;
                return result;
            }
            result.frame.commands.push_back(
                {.sequence = event.sequence,
                 .payload = ResourceConstructionCommand{.construction = construction->construction,
                                                        .retained = construction->retained}});
            ++result.frame.resourceConstructions;
            continue;
        }
        if (const auto *reset = std::get_if<RhiObservedVertexStreamReset>(&event.payload))
        {
            const BuildStatus status = EvidenceStatus(reset->evidence);
            if (status != BuildStatus::Accepted)
            {
                result.status = status;
                return result;
            }
            result.frame.commands.push_back(
                {.sequence = event.sequence,
                 .payload = VertexStreamResetCommand{.reset = reset->reset}});
            ++result.frame.vertexStreamResets;
            continue;
        }
        if (const auto *colorWrite = std::get_if<RhiObservedColorWriteState>(&event.payload))
        {
            const BuildStatus status = EvidenceStatus(colorWrite->evidence);
            if (status != BuildStatus::Accepted)
            {
                result.status = status;
                return result;
            }
            result.frame.commands.push_back(
                {.sequence = event.sequence,
                 .payload = ColorWriteStateCommand{.state = colorWrite->state}});
            ++result.frame.colorWriteStates;
            continue;
        }
        if (std::holds_alternative<RhiObservedViewport>(event.payload))
            continue;
        if (const auto *resolve = std::get_if<RhiObservedResolve>(&event.payload))
        {
            const BuildStatus status = EvidenceStatus(resolve->evidence);
            if (status != BuildStatus::Accepted)
            {
                result.status = status;
                return result;
            }
            result.frame.commands.push_back(
                {.sequence = event.sequence,
                 .payload = ResolveCommand{.resolve = resolve->resolve}});
            ++result.frame.resolves;
            continue;
        }

        const auto *present = std::get_if<RhiObservedPresent>(&event.payload);
        const BuildStatus status = EvidenceStatus(present->evidence);
        if (status != BuildStatus::Accepted)
        {
            result.status = status;
            return result;
        }
        if (havePresent)
        {
            result.status = BuildStatus::MultiplePresents;
            return result;
        }
        if (index + 1 != observed.events.size())
        {
            result.status = BuildStatus::PresentNotTerminal;
            return result;
        }
        havePresent = true;
        result.frame.commands.push_back(
            {.sequence = event.sequence, .payload = PresentCommand{.present = present->present}});
        ++result.frame.presents;
    }

    if (!havePresent)
    {
        result.status = BuildStatus::MissingPresent;
        return result;
    }
    result.frame.complete = true;
    result.status = BuildStatus::Accepted;
    return result;
}

const char *BuildStatusText(BuildStatus status) noexcept
{
    switch (status)
    {
    case BuildStatus::Accepted:
        return "accepted";
    case BuildStatus::Empty:
        return "empty";
    case BuildStatus::SequenceNotIncreasing:
        return "event sequence is not increasing";
    case BuildStatus::EvidenceMissing:
        return "required retained evidence is missing";
    case BuildStatus::EvidenceMismatch:
        return "retained evidence mismatches the semantic request";
    case BuildStatus::ConstructionEvidenceMissing:
        return "resource construction returned no wrapper evidence";
    case BuildStatus::MissingPresent:
        return "terminal present is missing";
    case BuildStatus::MultiplePresents:
        return "more than one present was observed";
    case BuildStatus::PresentNotTerminal:
        return "present is not the terminal command";
    }
    return "unknown";
}

bool PlanEnabled()
{
    static const bool enabled = lucent::config::flag("NATIVE_RHI_PLAN");
    return enabled;
}

void SubmitSemanticFrame(const RhiSemanticFrame &observed)
{
    if (!PlanEnabled())
        return;

    const BuildResult result = BuildFrame(observed);
    static std::atomic<std::uint64_t> acceptedFrames{0};
    if (result.status != BuildStatus::Accepted)
    {
        lucent::error("native",
                      "native RHI frame {} refused at event {}: {} -- host execution remains"
                      " disabled",
                      observed.frameSequence, result.eventIndex, BuildStatusText(result.status));
        return;
    }

    const std::uint64_t count = acceptedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || observed.frameSequence % 60 == 0)
    {
        lucent::info("native",
                     "native RHI frame plan accepted frame {}: {} command(s), {} draw(s),"
                     " {} binding(s), {} resolve(s), {} present(s); PM4-independent plan only",
                     observed.frameSequence, result.frame.commands.size(), result.frame.draws,
                     result.frame.bindings, result.frame.resolves, result.frame.presents);
    }
}

} // namespace gears::native_rhi
