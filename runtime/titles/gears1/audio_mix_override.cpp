#include "audio_mix.h"

#include "import_stub.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <lucent/config.h>
#include <lucent/log.h>

namespace
{

using RecompiledAudioMix = void (*)(PPCContext &, unsigned char *);

std::atomic<std::uint64_t> g_auditedCalls{0};
std::atomic<std::uint64_t> g_abCalls{0};

[[nodiscard]] bool NativeAudioMixRequested()
{
    static const bool requested = lucent::config::flag("NATIVE_AUDIO_MIX");
    return requested;
}

[[nodiscard]] bool RecompiledAudioMixRequested()
{
    static const bool requested = lucent::config::flag("RECOMP_AUDIO_MIX");
    return requested;
}

[[nodiscard]] bool AudioMixAbEnabled()
{
    static const bool enabled = lucent::config::flag("AUDIO_MIX_AB");
    return enabled;
}

[[nodiscard]] bool AudioMixAuditEnabled()
{
    static const bool enabled = lucent::config::flag("NATIVE_AUDIO_MIX_AUDIT");
    return enabled;
}

[[nodiscard]] std::uint64_t AudioMixAuditLimit()
{
    static const auto limit = static_cast<std::uint64_t>(
        std::max<long>(0, lucent::config::number("NATIVE_AUDIO_MIX_AUDIT_CALLS", 256)));
    return limit;
}

[[noreturn]] void FailAudit(std::size_t offset, std::uint8_t expected, std::uint8_t actual)
{
    lucent::error("audio",
                  "native audio mix diverged from retained 0x825F2D40 at output byte {:#x}"
                  " (expected {:#x}, actual {:#x})",
                  offset, expected, actual);
    std::abort();
}

std::uint64_t ReserveAudioMixAudit()
{
    const std::uint64_t limit = AudioMixAuditLimit();
    std::uint64_t ordinal = g_auditedCalls.load(std::memory_order_relaxed);
    while (ordinal < limit &&
           !g_auditedCalls.compare_exchange_weak(ordinal, ordinal + 1, std::memory_order_relaxed,
                                                 std::memory_order_relaxed))
    {
    }
    return ordinal < limit ? ordinal + 1 : 0;
}

void AuditNativeAudioMix(PPCContext &ctx, unsigned char *base, RecompiledAudioMix super,
                         std::uint64_t ordinal)
{
    constexpr std::size_t kOutputBytes = 1024;
    const PPCContext entryContext = ctx;
    const std::uint32_t output = ctx.r3.u32;
    std::array<std::uint8_t, kOutputBytes> before{};
    std::array<std::uint8_t, kOutputBytes> native{};
    std::memcpy(before.data(), base + output, before.size());

    gears::titles::gears1::ApplyNativeAudioMix(ctx, base);
    std::memcpy(native.data(), base + output, native.size());
    std::memcpy(base + output, before.data(), before.size());
    ctx = entryContext;
    super(ctx, base);
    for (std::size_t offset = 0; offset < native.size(); ++offset)
    {
        const std::uint8_t actual = base[output + offset];
        if (actual != native[offset])
            FailAudit(offset, native[offset], actual);
    }
    if (ordinal == AudioMixAuditLimit())
        lucent::info("audio", "native audio mix audit matched {} call(s)", ordinal);
}

} // namespace

extern "C" PPC_FUNC(__imp__sub_825F2D40);
PPC_FUNC(sub_825F2D40)
{
    const bool requested = NativeAudioMixRequested() && !RecompiledAudioMixRequested();
    const bool useNative =
        requested &&
        (!AudioMixAbEnabled() || (g_abCalls.fetch_add(1, std::memory_order_relaxed) & 1u) == 0);
    if (!useNative)
    {
        __imp__sub_825F2D40(ctx, base);
        return;
    }

    const std::uint64_t auditOrdinal = AudioMixAuditEnabled() ? ReserveAudioMixAudit() : 0;
    if (auditOrdinal != 0)
        AuditNativeAudioMix(ctx, base, __imp__sub_825F2D40, auditOrdinal);
    else
        gears::titles::gears1::ApplyNativeAudioMix(ctx, base);
}
