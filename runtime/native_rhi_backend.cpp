#include "native_rhi_backend.h"

#include <type_traits>
#include <variant>

namespace gears::native_rhi
{
namespace
{

template <typename Command> bool Dispatch(Backend &backend, const Command &command)
{
    if constexpr (std::is_same_v<Command, DrawCommand>)
        return backend.Draw(command);
    else if constexpr (std::is_same_v<Command, BindingCommand>)
        return backend.Bind(command);
    else if constexpr (std::is_same_v<Command, ResourceLifetimeCommand>)
        return backend.ApplyResourceLifetime(command);
    else if constexpr (std::is_same_v<Command, ResourceConstructionCommand>)
        return backend.ConstructResource(command);
    else if constexpr (std::is_same_v<Command, VertexStreamResetCommand>)
        return backend.ResetVertexStreams(command);
    else if constexpr (std::is_same_v<Command, ColorWriteStateCommand>)
        return backend.SetColorWriteState(command);
    else if constexpr (std::is_same_v<Command, ResolveCommand>)
        return backend.Resolve(command);
    else
        return backend.Present(command);
}

[[nodiscard]] bool IsPresent(const Command &command)
{
    return std::holds_alternative<PresentCommand>(command.payload);
}

} // namespace

ExecutionResult ExecuteFrame(const Frame &frame, Backend &backend)
{
    ExecutionResult result;
    if (!frame.complete || frame.commands.empty())
        return result;

    std::uint64_t previousSequence = 0;
    bool havePreviousSequence = false;
    bool havePresent = false;
    for (std::size_t index = 0; index < frame.commands.size(); ++index)
    {
        const Command &command = frame.commands[index];
        result.commandIndex = index;
        if (havePreviousSequence && command.sequence <= previousSequence)
        {
            result.status = ExecutionStatus::SequenceNotIncreasing;
            return result;
        }
        previousSequence = command.sequence;
        havePreviousSequence = true;

        const bool present = IsPresent(command);
        if (present && (havePresent || index + 1 != frame.commands.size()))
        {
            result.status = ExecutionStatus::PresentNotTerminal;
            return result;
        }
        if (!present && havePresent)
        {
            result.status = ExecutionStatus::PresentNotTerminal;
            return result;
        }
        havePresent = havePresent || present;
    }
    if (!havePresent)
        return result;

    if (!backend.BeginFrame(frame.frameSequence))
    {
        result.status = ExecutionStatus::BackendRefused;
        return result;
    }

    for (std::size_t index = 0; index < frame.commands.size(); ++index)
    {
        result.commandIndex = index;
        const bool accepted =
            std::visit([&backend](const auto &command) { return Dispatch(backend, command); },
                       frame.commands[index].payload);
        if (!accepted)
        {
            backend.CancelFrame();
            result.status = ExecutionStatus::BackendRefused;
            return result;
        }
    }

    if (!backend.FinishFrame())
    {
        backend.CancelFrame();
        result.status = ExecutionStatus::BackendRefused;
        return result;
    }
    result.status = ExecutionStatus::Accepted;
    return result;
}

const char *ExecutionStatusText(ExecutionStatus status) noexcept
{
    switch (status)
    {
    case ExecutionStatus::Accepted:
        return "accepted";
    case ExecutionStatus::IncompleteFrame:
        return "frame is incomplete or empty";
    case ExecutionStatus::SequenceNotIncreasing:
        return "command sequence is not increasing";
    case ExecutionStatus::PresentNotTerminal:
        return "present is not the terminal command";
    case ExecutionStatus::BackendRefused:
        return "native backend refused the frame";
    }
    return "unknown";
}

} // namespace gears::native_rhi
