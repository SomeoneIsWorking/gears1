#include "guest_state_memory.h"
#include "import_stub.h"
#include "rhi_resource_reference.h"
#include "rhi_semantic_stream.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <lucent/config.h>
#include <lucent/log.h>

namespace
{

using RecompiledLifetimeOperation = void (*)(PPCContext &, std::uint8_t *);

constexpr std::uint32_t kResourceFlagsOffset = 0;
constexpr std::uint32_t kReferenceCountOffset = 4;
constexpr std::uint32_t kBackingObjectOffset = 24;
constexpr std::uint32_t kResourceTypeMask = 0xF;

enum class ResourceReferenceExecution : std::uint8_t
{
    Native,
    Retained,
    BoundaryFallback,
};

struct ResourceReferenceTotals
{
    std::array<std::atomic<std::uint64_t>, 3> calls{};
    std::array<std::atomic<std::uint64_t>, 3> nanoseconds{};
    std::atomic<std::uint64_t> ordinal{0};
};

ResourceReferenceTotals g_totals;

[[nodiscard]] bool RecompiledResourceReferencesRequested()
{
    static const bool requested = lucent::config::flag("RECOMP_RESOURCE_REFERENCES");
    return requested;
}

[[nodiscard]] bool ResourceReferenceAbEnabled()
{
    static const bool enabled = lucent::config::flag("RESOURCE_REFERENCE_AB");
    return enabled;
}

[[nodiscard]] bool ResourceReferenceTimingEnabled()
{
    static const bool enabled = lucent::config::flag("RESOURCE_REFERENCE_TIMING");
    return enabled;
}

void RecordExecution(ResourceReferenceExecution execution, std::uint64_t nanoseconds,
                     std::uint64_t ordinal)
{
    const std::size_t index = static_cast<std::size_t>(execution);
    g_totals.calls[index].fetch_add(1, std::memory_order_relaxed);
    if (!ResourceReferenceTimingEnabled())
        return;
    g_totals.nanoseconds[index].fetch_add(nanoseconds, std::memory_order_relaxed);
    if (ordinal % 1'000 != 0)
        return;

    const auto mean = [](std::size_t sample)
    {
        const std::uint64_t calls = g_totals.calls[sample].load(std::memory_order_relaxed);
        return calls == 0 ? 0
                          : g_totals.nanoseconds[sample].load(std::memory_order_relaxed) / calls;
    };
    lucent::debug(
        "rhi",
        "resource reference execution through {} calls: native={} ({} ns mean), retained={}"
        " ({} ns mean), boundary fallback={} ({} ns mean)",
        ordinal, g_totals.calls[0].load(std::memory_order_relaxed), mean(0),
        g_totals.calls[1].load(std::memory_order_relaxed), mean(1),
        g_totals.calls[2].load(std::memory_order_relaxed), mean(2));
}

ResourceReferenceExecution ExecuteResourceReference(PPCContext &ctx, std::uint8_t *base,
                                                    gears::RhiResourceLifetimeOperation operation,
                                                    RecompiledLifetimeOperation retained)
{
    const std::uint64_t ordinal = g_totals.ordinal.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool selectNative = !RecompiledResourceReferencesRequested() &&
                              (!ResourceReferenceAbEnabled() || (ordinal & 1u) != 0);
    const bool timing = ResourceReferenceTimingEnabled();
    const auto started =
        timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    ResourceReferenceExecution execution = ResourceReferenceExecution::Retained;
    if (selectNative)
    {
        const auto result = gears::TryApplyNativeRhiReferenceFastPath(base, ctx.r3.u32, operation);
        if (result.has_value())
        {
            ctx.r3.u64 = *result;
            execution = ResourceReferenceExecution::Native;
        }
        else
        {
            retained(ctx, base);
            execution = ResourceReferenceExecution::BoundaryFallback;
        }
    }
    else
    {
        retained(ctx, base);
    }

    const auto elapsed =
        timing ? std::chrono::steady_clock::now() - started : std::chrono::steady_clock::duration{};
    RecordExecution(execution,
                    static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                    ordinal);
    return execution;
}

void ObserveResourceLifetime(PPCContext &ctx, std::uint8_t *base,
                             gears::RhiResourceLifetimeOperation operation,
                             RecompiledLifetimeOperation retained)
{
    const bool observe = gears::RhiSemanticObservationEnabled();
    if (!observe)
    {
        (void)ExecuteResourceReference(ctx, base, operation, retained);
        return;
    }

    gears::titles::gears1::GuestStateMemory memory(base);
    const std::uint32_t object = ctx.r3.u32;
    const std::uint32_t rawFlags = memory.Read32(object + kResourceFlagsOffset);
    const gears::RhiSemanticResourceLifetime lifetime{
        .operation = operation,
        .object = object,
        .rawFlags = rawFlags,
        .resourceType = rawFlags & kResourceTypeMask,
        .backingObject = memory.Read32(object + kBackingObjectOffset),
        .previousReferenceCount = memory.Read32(object + kReferenceCountOffset),
    };

    (void)ExecuteResourceReference(ctx, base, operation, retained);

    gears::ObserveRhiSemanticResourceLifetime(
        lifetime, {.present = true, .returnedReferenceCount = ctx.r3.u32});
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_8222E868);
PPC_FUNC(sub_8222E868)
{
    ObserveResourceLifetime(ctx, base, gears::RhiResourceLifetimeOperation::AddReference,
                            __imp__sub_8222E868);
}

extern "C" PPC_FUNC(__imp__sub_8222E8E0);
PPC_FUNC(sub_8222E8E0)
{
    ObserveResourceLifetime(ctx, base, gears::RhiResourceLifetimeOperation::Release,
                            __imp__sub_8222E8E0);
}
