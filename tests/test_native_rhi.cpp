#include "native_rhi.h"
#include "native_rhi_backend.h"

#include <cassert>
#include <cstdint>
#include <string_view>
#include <vector>

namespace
{

gears::RhiSemanticFrame CompleteFrame()
{
    const gears::RhiSemanticDrawState drawState{
        .draw = {.kind = gears::RhiSemanticDrawKind::TransientVertices,
                 .primitiveType = 4,
                 .elementCount = 3,
                 .vertexStrideBytes = 12,
                 .vertexData = {.guestAddress = 0x1000, .sizeBytes = 36}},
    };
    const gears::RhiObservedDraw draw{
        .state = drawState,
        .evidence = gears::RhiDrawEvidenceResult::Match,
    };
    const gears::RhiSemanticBinding binding{
        .kind = gears::RhiSemanticBindingKind::Texture,
        .slot = 0,
        .object = 0x2000,
    };
    const gears::RhiObservedBinding observedBinding{
        .binding = binding,
        .evidence = gears::RhiBindingEvidenceResult::Match,
    };
    const gears::RhiSemanticResourceLifetime lifetime{
        .operation = gears::RhiResourceLifetimeOperation::AddReference,
        .object = 0x3000,
        .resourceType = 4,
    };
    const gears::RhiObservedResourceLifetime observedLifetime{
        .lifetime = lifetime,
        .retained = {.present = true, .returnedReferenceCount = 2},
        .evidence = gears::RhiResourceLifetimeEvidenceResult::Match,
    };
    const gears::RhiObservedResourceConstruction construction{
        .construction = {.kind = gears::RhiSemanticResourceConstructionKind::OwnedBacking,
                         .requestedBytes = 64},
        .retained = {.present = true, .object = 0x4000},
    };
    const gears::RhiObservedVertexStreamReset reset{
        .reset = {.firstSlot = 0, .slotCount = 16},
        .state = {.present = true},
        .evidence = gears::RhiVertexStreamResetEvidenceResult::Match,
    };
    const gears::RhiObservedResolve resolve{
        .resolve = {.sourceObject = 0x5000, .destinationObject = 0x6000},
        .evidence = gears::RhiResolveEvidenceResult::Match,
    };
    const gears::RhiObservedPresent present{
        .present = {.frameSequence = 42, .frontBuffer = 0x7000},
        .evidence = gears::RhiPresentEvidenceResult::Match,
    };

    gears::RhiSemanticFrame frame{.frameSequence = 42};
    frame.events = {
        {.sequence = 10, .payload = draw},
        {.sequence = 11, .payload = observedBinding},
        {.sequence = 12, .payload = observedLifetime},
        {.sequence = 13, .payload = construction},
        {.sequence = 14, .payload = reset},
        {.sequence = 15, .payload = resolve},
        {.sequence = 16, .payload = present},
    };
    return frame;
}

} // namespace

class RecordingBackend final : public gears::native_rhi::Backend
{
  public:
    bool BeginFrame(std::uint64_t) override
    {
        calls.push_back("begin");
        return beginResult;
    }
    bool Bind(const gears::native_rhi::BindingCommand &) override
    {
        calls.push_back("binding");
        return commandResult;
    }
    bool ApplyResourceLifetime(const gears::native_rhi::ResourceLifetimeCommand &) override
    {
        calls.push_back("lifetime");
        return commandResult;
    }
    bool ConstructResource(const gears::native_rhi::ResourceConstructionCommand &) override
    {
        calls.push_back("construction");
        return commandResult;
    }
    bool ResetVertexStreams(const gears::native_rhi::VertexStreamResetCommand &) override
    {
        calls.push_back("reset");
        return commandResult;
    }
    bool Draw(const gears::native_rhi::DrawCommand &) override
    {
        calls.push_back("draw");
        return commandResult;
    }
    bool Resolve(const gears::native_rhi::ResolveCommand &) override
    {
        calls.push_back("resolve");
        return commandResult;
    }
    bool Present(const gears::native_rhi::PresentCommand &) override
    {
        calls.push_back("present");
        return commandResult;
    }
    bool FinishFrame() override
    {
        calls.push_back("finish");
        return finishResult;
    }
    void CancelFrame() override { calls.push_back("cancel"); }

    bool beginResult = true;
    bool commandResult = true;
    bool finishResult = true;
    std::vector<std::string_view> calls;
};

int main()
{
    const gears::native_rhi::BuildResult accepted = gears::native_rhi::BuildFrame(CompleteFrame());
    assert(accepted.status == gears::native_rhi::BuildStatus::Accepted);
    assert(accepted.frame.complete);
    assert(accepted.frame.frameSequence == 42);
    assert(accepted.frame.commands.size() == 7);
    assert(accepted.frame.draws == 1);
    assert(accepted.frame.bindings == 1);
    assert(accepted.frame.resourceLifetimeCalls == 1);
    assert(accepted.frame.resourceConstructions == 1);
    assert(accepted.frame.vertexStreamResets == 1);
    assert(accepted.frame.resolves == 1);
    assert(accepted.frame.presents == 1);
    assert(
        std::holds_alternative<gears::native_rhi::DrawCommand>(accepted.frame.commands[0].payload));
    assert(std::get<gears::native_rhi::DrawCommand>(accepted.frame.commands[0].payload)
               .state.draw.elementCount == 3);
    assert(std::holds_alternative<gears::native_rhi::PresentCommand>(
        accepted.frame.commands.back().payload));

    gears::RhiSemanticFrame missingEvidence = CompleteFrame();
    std::get<gears::RhiObservedDraw>(missingEvidence.events[0].payload).evidence =
        gears::RhiDrawEvidenceResult::Missing;
    assert(gears::native_rhi::BuildFrame(missingEvidence).status ==
           gears::native_rhi::BuildStatus::EvidenceMissing);

    gears::RhiSemanticFrame outOfOrder = CompleteFrame();
    outOfOrder.events[1].sequence = outOfOrder.events[0].sequence;
    assert(gears::native_rhi::BuildFrame(outOfOrder).status ==
           gears::native_rhi::BuildStatus::SequenceNotIncreasing);

    gears::RhiSemanticFrame noPresent = CompleteFrame();
    noPresent.events.pop_back();
    assert(gears::native_rhi::BuildFrame(noPresent).status ==
           gears::native_rhi::BuildStatus::MissingPresent);

    gears::RhiSemanticFrame lateCommand = CompleteFrame();
    lateCommand.events.push_back(lateCommand.events[1]);
    lateCommand.events.back().sequence = 17;
    assert(gears::native_rhi::BuildFrame(lateCommand).status ==
           gears::native_rhi::BuildStatus::PresentNotTerminal);

    gears::RhiSemanticFrame missingConstruction = CompleteFrame();
    std::get<gears::RhiObservedResourceConstruction>(missingConstruction.events[3].payload)
        .retained.present = false;
    assert(gears::native_rhi::BuildFrame(missingConstruction).status ==
           gears::native_rhi::BuildStatus::ConstructionEvidenceMissing);
    assert(std::string_view(gears::native_rhi::BuildStatusText(
               gears::native_rhi::BuildStatus::Accepted)) == "accepted");

    RecordingBackend backend;
    const gears::native_rhi::ExecutionResult executed =
        gears::native_rhi::ExecuteFrame(accepted.frame, backend);
    assert(executed.status == gears::native_rhi::ExecutionStatus::Accepted);
    assert((backend.calls == std::vector<std::string_view>{"begin", "draw", "binding", "lifetime",
                                                           "construction", "reset", "resolve",
                                                           "present", "finish"}));

    RecordingBackend refusingBackend;
    refusingBackend.commandResult = false;
    const gears::native_rhi::ExecutionResult refused =
        gears::native_rhi::ExecuteFrame(accepted.frame, refusingBackend);
    assert(refused.status == gears::native_rhi::ExecutionStatus::BackendRefused);
    assert(refused.commandIndex == 0);
    assert((refusingBackend.calls == std::vector<std::string_view>{"begin", "draw", "cancel"}));

    RecordingBackend incompleteBackend;
    gears::native_rhi::Frame incomplete;
    const gears::native_rhi::ExecutionResult incompleteResult =
        gears::native_rhi::ExecuteFrame(incomplete, incompleteBackend);
    assert(incompleteResult.status == gears::native_rhi::ExecutionStatus::IncompleteFrame);
    assert(incompleteBackend.calls.empty());

    gears::native_rhi::Frame malformed = accepted.frame;
    malformed.commands[1].sequence = malformed.commands[0].sequence;
    RecordingBackend malformedBackend;
    const gears::native_rhi::ExecutionResult malformedResult =
        gears::native_rhi::ExecuteFrame(malformed, malformedBackend);
    assert(malformedResult.status == gears::native_rhi::ExecutionStatus::SequenceNotIncreasing);
    assert(malformedBackend.calls.empty());
}
