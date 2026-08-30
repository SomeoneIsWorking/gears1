#include "color_write_gamma_state.h"
#include "guest_state_memory.h"
#include "import_stub.h"
#include "rhi_semantic_stream.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <lucent/config.h>
#include <lucent/log.h>

namespace
{

using gears::titles::gears1::ApplyNativeColorWriteGammaState;
using gears::titles::gears1::GuestStateMemory;
using RecompiledSetter = void (*)(PPCContext &, std::uint8_t *);

enum class ExecutionKind : std::uint8_t
{
    Native,
    Recompiled,
};

struct TimingSummary
{
    std::size_t calls = 0;
    std::uint64_t medianNanoseconds = 0;
    std::uint64_t p95Nanoseconds = 0;
};

struct Totals
{
    std::atomic<std::uint64_t> auditedCalls{0};
    std::atomic<std::uint64_t> auditedTransitions{0};
    std::atomic<std::uint64_t> calls{0};
    std::array<std::vector<std::uint64_t>, 2> timingSamples;
    std::mutex timingMutex;
};

Totals g_totals;
std::atomic<std::uint64_t> g_auditOrdinal{0};
std::atomic<std::uint64_t> g_abOrdinal{0};
std::once_flag g_configOnce;
std::atomic<bool> g_configInitialized{false};
bool g_recompiledRequested = false;
bool g_nativeRequested = false;
bool g_auditEnabled = false;
bool g_abEnabled = false;
bool g_timingEnabled = false;
bool g_overrideOrDiagnosticsRequested = false;
bool g_semanticObservationEnabled = false;
std::uint64_t g_auditLimit = 0;

void ObserveColorWriteState(std::uint8_t *base, std::uint32_t device, std::uint64_t requested)
{
    GuestStateMemory memory(base);
    gears::RhiSemanticColorWriteState state{
        .requested = requested,
        .surfaceStatePresent = device != 0,
    };
    if (device != 0)
    {
        state.surfaceState = gears::DecodeRhiSurfaceState(
            memory.Read32(device + gears::titles::gears1::color_write_gamma::kSurfaceInfoOffset));
        state.target.object = memory.Read32(
            device + gears::titles::gears1::color_write_gamma::kColorTargetObjectOffset);
        state.targetPresent = state.target.object != 0;
        if (state.targetPresent)
        {
            state.target.descriptor = {memory.Read32(
                device + gears::titles::gears1::color_write_gamma::kColorDescriptorOffset)};
            state.target.descriptorDwords = 1;
            state.target.normalizedStatePresent = true;
            state.target.normalizedState =
                gears::DecodeRhiColorTargetDescriptor(state.target.descriptor[0]);
        }
    }
    gears::ObserveRhiSemanticColorWriteState(state);
}

[[nodiscard]] bool SameRequiredContext(const PPCContext &expected, const PPCContext &actual)
{
    return expected.r1.u64 == actual.r1.u64 && expected.r28.u64 == actual.r28.u64 &&
           expected.r29.u64 == actual.r29.u64 && expected.r30.u64 == actual.r30.u64 &&
           expected.r31.u64 == actual.r31.u64 && expected.lr == actual.lr;
}

[[noreturn]] void FailAudit(std::uint32_t device, std::uint64_t requested, const char *region,
                            std::uint32_t address = 0, std::uint8_t expected = 0,
                            std::uint8_t actual = 0)
{
    lucent::error("rhi",
                  "native color-write gamma setter diverged from retained recomp body at {}"
                  " (device {:#x}, requested {:#x}, address {:#x}, expected {:#x}, actual {:#x})",
                  region, device, requested, address, expected, actual);
    std::abort();
}

void AuditNativeSetter(PPCContext &ctx, std::uint8_t *base, RecompiledSetter super,
                       std::uint32_t device, std::uint64_t requested)
{
    const PPCContext entryContext = ctx;
    std::vector<GuestStateMemory::ByteChange> changes;
    GuestStateMemory memory(base, &changes);
    const bool transitioned = ApplyNativeColorWriteGammaState(memory, device, requested);
    for (auto &change : changes)
        change.after = memory.Read8(change.address);
    for (const auto &change : changes)
        memory.Write8(change.address, change.before);

    ctx = entryContext;
    super(ctx, base);
    if (!SameRequiredContext(entryContext, ctx))
        FailAudit(device, requested, "callee-saved context");
    for (const auto &change : changes)
    {
        const std::uint8_t actual = memory.Read8(change.address);
        if (actual != change.after)
            FailAudit(device, requested, "guest state", change.address, change.after, actual);
    }
    if (transitioned)
        g_totals.auditedTransitions.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t auditedCalls =
        g_totals.auditedCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (auditedCalls == g_auditLimit)
    {
        lucent::info("rhi", "color-write gamma audit matched {} call(s), {} state transition(s)",
                     auditedCalls, g_totals.auditedTransitions.load(std::memory_order_relaxed));
    }
}

[[nodiscard]] TimingSummary SummarizeTiming(std::vector<std::uint64_t> samples)
{
    if (samples.empty())
        return {};
    std::ranges::sort(samples);
    return {
        .calls = samples.size(),
        .medianNanoseconds = samples[samples.size() / 2],
        .p95Nanoseconds = samples[(samples.size() - 1) * 95 / 100],
    };
}

void MaybeReport(std::uint64_t calls)
{
    constexpr std::uint64_t kReportInterval = 10'000;
    if ((!g_timingEnabled && !g_auditEnabled) || calls % kReportInterval != 0)
        return;
    std::array<std::vector<std::uint64_t>, 2> samples;
    {
        std::scoped_lock lock(g_totals.timingMutex);
        samples = g_totals.timingSamples;
    }
    const TimingSummary native = SummarizeTiming(std::move(samples[0]));
    const TimingSummary recompiled = SummarizeTiming(std::move(samples[1]));
    lucent::info("rhi",
                 "color-write gamma through call {}: native {} median {} ns p95 {} ns, recomp"
                 " control {} median {} ns p95 {} ns, {} same-run audit match(es), {} state"
                 " transition(s)",
                 calls, native.calls, native.medianNanoseconds, native.p95Nanoseconds,
                 recompiled.calls, recompiled.medianNanoseconds, recompiled.p95Nanoseconds,
                 g_totals.auditedCalls.load(std::memory_order_relaxed),
                 g_totals.auditedTransitions.load(std::memory_order_relaxed));
}

[[gnu::noinline]] void ExecuteSetter(PPCContext &ctx, std::uint8_t *base, RecompiledSetter super)
{
    const std::uint32_t device = ctx.r3.u32;
    const std::uint64_t requested = ctx.r4.u64;
    const bool nativeEligible =
        !g_recompiledRequested && (g_nativeRequested || g_auditEnabled || g_abEnabled);
    const bool useNative =
        nativeEligible &&
        (!g_abEnabled || (g_abOrdinal.fetch_add(1, std::memory_order_relaxed) & 1u) == 0);
    const bool timing = g_timingEnabled;
    const auto started =
        timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    bool audited = false;

    if (!useNative)
    {
        super(ctx, base);
    }
    else if (g_auditEnabled &&
             g_auditOrdinal.fetch_add(1, std::memory_order_relaxed) < g_auditLimit)
    {
        AuditNativeSetter(ctx, base, super, device, requested);
        audited = true;
    }
    else
    {
        GuestStateMemory memory(base);
        (void)ApplyNativeColorWriteGammaState(memory, device, requested);
    }

    if (timing && !audited)
    {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        std::scoped_lock lock(g_totals.timingMutex);
        g_totals
            .timingSamples[static_cast<std::size_t>(useNative ? ExecutionKind::Native
                                                              : ExecutionKind::Recompiled)]
            .push_back(nanoseconds);
    }
    MaybeReport(g_totals.calls.fetch_add(1, std::memory_order_relaxed) + 1);
}

[[gnu::noinline]] void InitializeConfigAndExecute(PPCContext &ctx, std::uint8_t *base,
                                                  RecompiledSetter super)
{
    std::call_once(g_configOnce,
                   []
                   {
                       g_recompiledRequested = lucent::config::flag("RECOMP_COLOR_WRITE_GAMMA");
                       g_nativeRequested = lucent::config::flag("NATIVE_COLOR_WRITE_GAMMA");
                       g_auditEnabled = lucent::config::flag("NATIVE_COLOR_WRITE_GAMMA_AUDIT");
                       g_abEnabled = lucent::config::flag("COLOR_WRITE_GAMMA_AB");
                       g_timingEnabled = lucent::config::flag("COLOR_WRITE_GAMMA_TIMING");
                       g_overrideOrDiagnosticsRequested =
                           g_nativeRequested || g_auditEnabled || g_abEnabled || g_timingEnabled;
                       g_semanticObservationEnabled = gears::RhiSemanticObservationEnabled();
                       g_auditLimit = static_cast<std::uint64_t>(std::max<long>(
                           0, lucent::config::number("NATIVE_COLOR_WRITE_GAMMA_AUDIT_CALLS", 256)));
                       g_configInitialized.store(true, std::memory_order_release);
                   });
    if (g_overrideOrDiagnosticsRequested)
        ExecuteSetter(ctx, base, super);
    else
        super(ctx, base);
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_82229B28);
PPC_FUNC(sub_82229B28)
{
    const std::uint32_t device = ctx.r3.u32;
    const std::uint64_t requested = ctx.r4.u64;
    if (!g_configInitialized.load(std::memory_order_acquire))
        InitializeConfigAndExecute(ctx, base, __imp__sub_82229B28);
    else if (!g_overrideOrDiagnosticsRequested)
        __imp__sub_82229B28(ctx, base);
    else
        ExecuteSetter(ctx, base, __imp__sub_82229B28);

    if (g_semanticObservationEnabled)
        ObserveColorWriteState(base, device, requested);
}
