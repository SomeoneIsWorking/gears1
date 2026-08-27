#include "guest_memory.h"
#include "import_stub.h"
#include "rhi_semantic_stream.h"
#include "shader_setter_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <lucent/config.h>
#include <lucent/log.h>

namespace
{

using gears::titles::gears1::ShaderSetterSpecFor;
using gears::titles::gears1::ShaderStage;

class GuestShaderSetterMemory
{
  public:
    struct ByteChange
    {
        std::uint32_t address = 0;
        std::uint8_t before = 0;
        std::uint8_t after = 0;
    };

    explicit GuestShaderSetterMemory(std::uint8_t *base, std::vector<ByteChange> *changes = nullptr)
        : base_(base), changes_(changes)
    {
    }

    [[nodiscard]] std::uint8_t Read8(std::uint32_t address) const { return base_[address]; }

    [[nodiscard]] std::uint16_t Read16(std::uint32_t address) const
    {
        std::uint16_t value = 0;
        std::memcpy(&value, base_ + address, sizeof(value));
        return __builtin_bswap16(value);
    }

    [[nodiscard]] std::uint32_t Read32(std::uint32_t address) const
    {
        std::uint32_t value = 0;
        std::memcpy(&value, base_ + address, sizeof(value));
        return __builtin_bswap32(value);
    }

    [[nodiscard]] std::uint64_t Read64(std::uint32_t address) const
    {
        std::uint64_t value = 0;
        std::memcpy(&value, base_ + address, sizeof(value));
        return __builtin_bswap64(value);
    }

    void Write8(std::uint32_t address, std::uint8_t value)
    {
        RecordOriginal(address, 1);
        base_[address] = value;
    }

    void Write32(std::uint32_t address, std::uint32_t value)
    {
        RecordOriginal(address, sizeof(value));
        value = __builtin_bswap32(value);
        std::memcpy(base_ + address, &value, sizeof(value));
    }

    void Write64(std::uint32_t address, std::uint64_t value)
    {
        RecordOriginal(address, sizeof(value));
        value = __builtin_bswap64(value);
        std::memcpy(base_ + address, &value, sizeof(value));
    }

    void CopyBytesForward(std::uint32_t destination, std::uint32_t source, std::uint32_t bytes)
    {
        RecordOriginal(destination, bytes);
        auto *destinationBytes = base_ + destination;
        const auto *sourceBytes = base_ + source;
        if (destination > source && destination - source < bytes)
        {
            for (std::uint32_t byte = 0; byte < bytes; ++byte)
                destinationBytes[byte] = sourceBytes[byte];
            return;
        }
        std::memcpy(destinationBytes, sourceBytes, bytes);
    }

  private:
    void RecordOriginal(std::uint32_t address, std::size_t size)
    {
        if (changes_ == nullptr)
            return;
        for (std::size_t byte = 0; byte < size; ++byte)
        {
            const std::uint32_t byteAddress = address + static_cast<std::uint32_t>(byte);
            const auto existing = std::ranges::find(*changes_, byteAddress, &ByteChange::address);
            if (existing == changes_->end())
                changes_->push_back({.address = byteAddress, .before = Read8(byteAddress)});
        }
    }

    std::uint8_t *base_ = nullptr;
    std::vector<ByteChange> *changes_ = nullptr;
};

struct ShaderSetterTotals
{
    std::atomic<std::uint64_t> auditedCalls{0};
    std::mutex timingMutex;
    std::array<std::vector<std::uint64_t>, 3> timingSamples;
};

ShaderSetterTotals g_totals;
std::atomic<std::uint64_t> g_auditOrdinal{0};
std::array<std::atomic<std::uint64_t>, 2> g_abOrdinals{};
std::atomic<std::uint64_t> g_timingOrdinal{0};
std::atomic<std::uint64_t> g_reportOrdinal{0};

[[nodiscard]] bool RecompShaderSettersRequested()
{
    static const bool requested = lucent::config::flag("RECOMP_SHADER_SETTERS");
    return requested;
}

[[nodiscard]] bool ShaderSetterAuditEnabled()
{
    static const bool enabled = lucent::config::flag("NATIVE_SHADER_SETTER_AUDIT");
    return enabled;
}

[[nodiscard]] bool ShaderSetterTimingEnabled()
{
    static const bool enabled = lucent::config::flag("SHADER_SETTER_TIMING");
    return enabled;
}

[[nodiscard]] bool ShaderSetterAbEnabled()
{
    static const bool enabled = lucent::config::flag("SHADER_SETTER_AB");
    return enabled;
}

[[nodiscard]] bool NativeShaderSettersRequested()
{
    static const bool requested = lucent::config::flag("NATIVE_SHADER_SETTERS");
    return requested || ShaderSetterAuditEnabled() || ShaderSetterAbEnabled();
}

[[nodiscard]] std::uint64_t ShaderSetterAuditLimit()
{
    static const auto limit = static_cast<std::uint64_t>(
        std::max<long>(0, lucent::config::number("NATIVE_SHADER_SETTER_AUDIT_CALLS", 256)));
    return limit;
}

[[nodiscard]] std::uint64_t ShaderSetterTimingWarmupCalls()
{
    static const auto calls = static_cast<std::uint64_t>(
        std::max<long>(0, lucent::config::number("SHADER_SETTER_TIMING_WARMUP_CALLS", 128)));
    return calls;
}

[[nodiscard]] std::uint32_t ReadGuestBe32(std::uint32_t address)
{
    return GuestShaderSetterMemory{gears::Memory().Base()}.Read32(address);
}

[[nodiscard]] gears::RhiBindingStateEvidence CaptureShaderBinding(std::uint32_t device,
                                                                  std::uint32_t fieldOffset)
{
    if (device == 0)
        return {};
    return {
        .present = true,
        .observedObject = ReadGuestBe32(device + fieldOffset),
    };
}

using RecompiledSetter = void (*)(PPCContext &, std::uint8_t *);

enum class ExecutionKind : std::uint8_t
{
    Native,
    Recompiled,
    Fallback,
};

void MaybeReportShaderSetterTiming(std::uint64_t callSequence);

void RecordExecution(ExecutionKind kind, std::uint64_t nanoseconds)
{
    if (!ShaderSetterTimingEnabled())
        return;
    const auto index = static_cast<std::size_t>(kind);
    std::scoped_lock lock(g_totals.timingMutex);
    g_totals.timingSamples[index].push_back(nanoseconds);
}

[[nodiscard]] bool SameRequiredContext(const PPCContext &expected, const PPCContext &actual)
{
    return expected.r1.u64 == actual.r1.u64 && expected.r28.u64 == actual.r28.u64 &&
           expected.r29.u64 == actual.r29.u64 && expected.r30.u64 == actual.r30.u64 &&
           expected.r31.u64 == actual.r31.u64 && expected.lr == actual.lr;
}

[[noreturn]] void FailAudit(ShaderStage stage, std::uint32_t device, std::uint32_t shader,
                            const char *region, std::size_t byteOffset, std::uint32_t expected = 0,
                            std::uint32_t actual = 0)
{
    lucent::error("rhi",
                  "native {} shader setter diverged from retained recomp body at {} byte {:#x}"
                  " (device {:#x}, shader {:#x}, expected {:#x}, actual {:#x})",
                  stage == ShaderStage::Pixel ? "pixel" : "vertex", region, byteOffset, device,
                  shader, expected, actual);
    std::abort();
}

void AuditNativeSetter(PPCContext &ctx, std::uint8_t *base, ShaderStage stage,
                       RecompiledSetter super, std::uint32_t device, std::uint32_t shader)
{
    const PPCContext entryContext = ctx;
    std::vector<GuestShaderSetterMemory::ByteChange> changes;
    GuestShaderSetterMemory memory(base, &changes);
    gears::titles::gears1::ApplyNativeShaderSetter(memory, stage, device, shader);
    for (auto &change : changes)
        change.after = memory.Read8(change.address);
    for (const auto &change : changes)
        memory.Write8(change.address, change.before);

    ctx = entryContext;
    super(ctx, base);
    if (!SameRequiredContext(entryContext, ctx))
        FailAudit(stage, device, shader, "callee-saved context", 0);
    for (const auto &change : changes)
    {
        const std::uint8_t actual = memory.Read8(change.address);
        const std::array<std::uint8_t, 1> expectedBytes{change.after};
        const std::array<std::uint8_t, 1> actualBytes{actual};
        if (gears::titles::gears1::FirstShaderSetterStateMismatch(expectedBytes, actualBytes))
            FailAudit(stage, device, shader, "guest state", change.address - device, change.after,
                      actual);
    }
    g_totals.auditedCalls.fetch_add(1, std::memory_order_relaxed);
}

void ExecuteShaderSetter(PPCContext &ctx, std::uint8_t *base, ShaderStage stage,
                         RecompiledSetter super)
{
    const std::uint32_t device = ctx.r3.u32;
    const std::uint32_t shader = ctx.r4.u32;
    GuestShaderSetterMemory memory(base);
    const bool timing = ShaderSetterTimingEnabled();
    const auto started =
        timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const bool forceRecompiled = RecompShaderSettersRequested() || !NativeShaderSettersRequested();
    const bool nativeEligible =
        !forceRecompiled &&
        gears::titles::gears1::CanApplyNativeShaderSetter(memory, stage, device, shader);
    const std::size_t stageIndex = stage == ShaderStage::Pixel ? 0 : 1;
    const bool useNative =
        nativeEligible &&
        (!ShaderSetterAbEnabled() ||
         (g_abOrdinals[stageIndex].fetch_add(1, std::memory_order_relaxed) & 1u) == 0);
    const ExecutionKind kind = forceRecompiled || (nativeEligible && !useNative)
                                   ? ExecutionKind::Recompiled
                               : useNative ? ExecutionKind::Native
                                           : ExecutionKind::Fallback;

    if (!useNative)
    {
        super(ctx, base);
    }
    else if (ShaderSetterAuditEnabled() &&
             g_auditOrdinal.fetch_add(1, std::memory_order_relaxed) < ShaderSetterAuditLimit())
    {
        AuditNativeSetter(ctx, base, stage, super, device, shader);
    }
    else
    {
        gears::titles::gears1::ApplyNativeShaderSetter(memory, stage, device, shader);
    }

    if (timing &&
        g_timingOrdinal.fetch_add(1, std::memory_order_relaxed) >= ShaderSetterTimingWarmupCalls())
    {
        const auto elapsed = std::chrono::steady_clock::now() - started;
        RecordExecution(kind,
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
    }
    if (timing || ShaderSetterAuditEnabled())
    {
        const std::uint64_t callSequence =
            g_reportOrdinal.fetch_add(1, std::memory_order_relaxed) + 1;
        MaybeReportShaderSetterTiming(callSequence);
    }
}

void ObserveShaderBinding(ShaderStage stage, std::uint32_t device, std::uint32_t shader)
{
    const auto kind = stage == ShaderStage::Pixel ? gears::RhiSemanticBindingKind::PixelShader
                                                  : gears::RhiSemanticBindingKind::VertexShader;
    const auto spec = ShaderSetterSpecFor(stage);
    gears::ObserveRhiSemanticBinding({.kind = kind, .object = shader},
                                     CaptureShaderBinding(device, spec.deviceShaderOffset));
}

struct TimingSummary
{
    std::size_t calls = 0;
    std::uint64_t medianNanoseconds = 0;
    std::uint64_t p95Nanoseconds = 0;
};

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

void MaybeReportShaderSetterTiming(std::uint64_t callSequence)
{
    if ((!ShaderSetterTimingEnabled() && !ShaderSetterAuditEnabled()) || callSequence % 120 != 0)
        return;

    std::array<std::vector<std::uint64_t>, 3> samples;
    {
        std::scoped_lock lock(g_totals.timingMutex);
        samples = g_totals.timingSamples;
    }
    const auto native = SummarizeTiming(std::move(samples[0]));
    const auto recompiled = SummarizeTiming(std::move(samples[1]));
    const auto fallback = SummarizeTiming(std::move(samples[2]));
    lucent::info(
        "rhi",
        "shader setters through call {}: native {} median {} ns p95 {} ns, recomp control {}"
        " median {} ns p95 {} ns, recomp fallback {} median {} ns p95 {} ns, {} same-run audit"
        " match(es)",
        callSequence, native.calls, native.medianNanoseconds, native.p95Nanoseconds,
        recompiled.calls, recompiled.medianNanoseconds, recompiled.p95Nanoseconds, fallback.calls,
        fallback.medianNanoseconds, fallback.p95Nanoseconds,
        g_totals.auditedCalls.load(std::memory_order_relaxed));
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_82222808);
PPC_FUNC(sub_82222808)
{
    const std::uint32_t device = ctx.r3.u32;
    const std::uint32_t shader = ctx.r4.u32;
    ExecuteShaderSetter(ctx, base, ShaderStage::Pixel, __imp__sub_82222808);
    if (gears::RhiSemanticObservationEnabled())
        ObserveShaderBinding(ShaderStage::Pixel, device, shader);
}

extern "C" PPC_FUNC(__imp__sub_82222B98);
PPC_FUNC(sub_82222B98)
{
    const std::uint32_t device = ctx.r3.u32;
    const std::uint32_t shader = ctx.r4.u32;
    ExecuteShaderSetter(ctx, base, ShaderStage::Vertex, __imp__sub_82222B98);
    if (gears::RhiSemanticObservationEnabled())
        ObserveShaderBinding(ShaderStage::Vertex, device, shader);
}
