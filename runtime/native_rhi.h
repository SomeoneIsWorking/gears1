#pragma once

#include "rhi_semantic_stream.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace gears::native_rhi
{

// These commands intentionally contain RHI semantics only. They do not carry
// PM4 packets, Xenos registers, EDRAM tile state, or translated shader
// microcode. A future host backend consumes this plan and owns its own native
// resource, pipeline, and presentation objects.
struct DrawCommand
{
    RhiSemanticDrawState state;
};

struct BindingCommand
{
    RhiSemanticBinding binding;
};

struct ResourceLifetimeCommand
{
    RhiSemanticResourceLifetime lifetime;
};

struct ResourceConstructionCommand
{
    RhiSemanticResourceConstruction construction;
    RhiResourceConstructionEvidence retained;
};

struct VertexStreamResetCommand
{
    RhiSemanticVertexStreamReset reset;
};

struct ResolveCommand
{
    RhiSemanticResolve resolve;
};

struct PresentCommand
{
    RhiSemanticPresent present;
};

using CommandPayload =
    std::variant<DrawCommand, BindingCommand, ResourceLifetimeCommand, ResourceConstructionCommand,
                 VertexStreamResetCommand, ResolveCommand, PresentCommand>;

struct Command
{
    std::uint64_t sequence = 0;
    CommandPayload payload;
};

struct Frame
{
    std::uint64_t frameSequence = 0;
    std::vector<Command> commands;
    std::uint64_t draws = 0;
    std::uint64_t bindings = 0;
    std::uint64_t resourceLifetimeCalls = 0;
    std::uint64_t resourceConstructions = 0;
    std::uint64_t vertexStreamResets = 0;
    std::uint64_t resolves = 0;
    std::uint64_t presents = 0;
    bool complete = false;
};

enum class BuildStatus : std::uint8_t
{
    Accepted,
    Empty,
    SequenceNotIncreasing,
    EvidenceMissing,
    EvidenceMismatch,
    ConstructionEvidenceMissing,
    MissingPresent,
    MultiplePresents,
    PresentNotTerminal,
};

struct BuildResult
{
    Frame frame;
    BuildStatus status = BuildStatus::Empty;
    std::size_t eventIndex = 0;
};

[[nodiscard]] BuildResult BuildFrame(const RhiSemanticFrame &observed);
[[nodiscard]] const char *BuildStatusText(BuildStatus status) noexcept;

// This is a plan/capture boundary, not a renderer toggle. It proves that a
// native owner can receive a complete ordered frame without reading PM4 state;
// host execution remains deliberately disabled until its parity gate exists.
[[nodiscard]] bool PlanEnabled();
void SubmitSemanticFrame(const RhiSemanticFrame &observed);
void ObserveAndSubmitPresent(const RhiSemanticPresent &present,
                             const RhiPresentPacketEvidence &packet);

} // namespace gears::native_rhi
