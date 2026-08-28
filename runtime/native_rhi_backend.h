#pragma once

#include "native_rhi.h"

#include <cstddef>
#include <cstdint>

namespace gears::native_rhi
{

// A backend owns native resources, pipeline selection, submission, and
// retirement. The executor owns only command ordering and the transaction
// boundary; it never interprets PM4 or reaches into the compatibility renderer.
class Backend
{
  public:
    virtual ~Backend() = default;

    virtual bool BeginFrame(std::uint64_t frameSequence) = 0;
    virtual bool Bind(const BindingCommand &command) = 0;
    virtual bool ApplyResourceLifetime(const ResourceLifetimeCommand &command) = 0;
    virtual bool ConstructResource(const ResourceConstructionCommand &command) = 0;
    virtual bool ResetVertexStreams(const VertexStreamResetCommand &command) = 0;
    virtual bool Draw(const DrawCommand &command) = 0;
    virtual bool Resolve(const ResolveCommand &command) = 0;
    virtual bool Present(const PresentCommand &command) = 0;
    virtual bool FinishFrame() = 0;

    // A backend may have changed native state before refusing a command. It
    // must use this hook to discard that partial frame before the next one.
    virtual void CancelFrame() = 0;
};

enum class ExecutionStatus : std::uint8_t
{
    Accepted,
    IncompleteFrame,
    SequenceNotIncreasing,
    PresentNotTerminal,
    BackendRefused,
};

struct ExecutionResult
{
    ExecutionStatus status = ExecutionStatus::IncompleteFrame;
    std::size_t commandIndex = 0;
};

// Dispatches one validated, PM4-independent frame to a native host backend.
// No production backend is installed yet; callers must provide one explicitly
// so the compatibility path cannot be bypassed by an accidental no-op.
[[nodiscard]] ExecutionResult ExecuteFrame(const Frame &frame, Backend &backend);
[[nodiscard]] const char *ExecutionStatusText(ExecutionStatus status) noexcept;

} // namespace gears::native_rhi
