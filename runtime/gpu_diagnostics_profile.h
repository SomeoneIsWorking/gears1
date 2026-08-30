#pragma once

#include <cstdint>

namespace gears
{

struct GpuGraphicsInterruptDiagnostics
{
    std::uint32_t workerPoolGlobal = 0;
    std::uint32_t eventArrayOffset = 0;
    std::uint32_t eventStride = 0;
};

struct GpuRepresentativeDrawDiagnostics
{
    std::uint64_t vertexShaderHash = 0;
    std::uint32_t vertexFetchIndex = 0;
    std::uint32_t vertexStrideDwords = 0;
};

struct GpuDiagnosticsProfile
{
    GpuGraphicsInterruptDiagnostics graphicsInterrupt{};
    GpuRepresentativeDrawDiagnostics representativeDraw{};
};

struct GpuGraphicsInterruptDiagnosticState
{
    std::uint32_t workerPool = 0;
    std::uint32_t event = 0;
};

// The Xenos fetch file contains 96 two-dword vertex-fetch constants. Other
// fields are required because a zero address/hash/stride cannot identify the
// exact title state these diagnostics describe. An event-array offset of zero
// is valid for a title whose array begins at its worker-pool base.
[[nodiscard]] constexpr bool
IsValidGpuDiagnosticsProfile(const GpuDiagnosticsProfile &profile) noexcept
{
    return profile.graphicsInterrupt.workerPoolGlobal != 0 &&
           profile.graphicsInterrupt.eventStride != 0 &&
           profile.representativeDraw.vertexShaderHash != 0 &&
           profile.representativeDraw.vertexFetchIndex < 96 &&
           profile.representativeDraw.vertexStrideDwords != 0;
}

[[nodiscard]] constexpr std::uint32_t
GpuGraphicsInterruptEventAddress(const GpuGraphicsInterruptDiagnostics &layout,
                                 std::uint32_t workerPool, std::uint32_t cpu) noexcept
{
    return workerPool + layout.eventArrayOffset + cpu * layout.eventStride;
}

// One executable links one exact title adapter. Each adapter supplies this
// strong definition; accidentally linking two adapters is therefore a link
// error rather than an ambiguous runtime selection.
[[nodiscard]] const GpuDiagnosticsProfile &LinkedGpuDiagnosticsProfile();

// Focused shared application of the linked profile. Invalid profiles refuse;
// the command processor retains only generic orchestration and reporting.
[[nodiscard]] const GpuRepresentativeDrawDiagnostics *CurrentGpuRepresentativeDrawDiagnostics();
[[nodiscard]] bool
ResolveGpuGraphicsInterruptDiagnostics(std::uint32_t cpu,
                                       GpuGraphicsInterruptDiagnosticState &state);

} // namespace gears
