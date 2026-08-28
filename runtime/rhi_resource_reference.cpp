#include "rhi_resource_reference.h"

#include <atomic>

namespace gears
{

std::optional<std::uint32_t>
TryApplyNativeRhiReferenceFastPath(std::uint8_t *guestBase, std::uint32_t object,
                                   RhiResourceLifetimeOperation operation)
{
    constexpr std::uint32_t kReferenceCountOffset = 4;
    if (guestBase == nullptr || object == 0)
        return std::nullopt;

    auto *storage = reinterpret_cast<std::uint32_t *>(guestBase + object + kReferenceCountOffset);
    std::atomic_ref<std::uint32_t> reference(*storage);
    std::uint32_t expectedRaw = reference.load(std::memory_order_seq_cst);
    for (;;)
    {
        const std::uint32_t previous = __builtin_bswap32(expectedRaw);
        if ((operation == RhiResourceLifetimeOperation::AddReference && previous == 0) ||
            (operation == RhiResourceLifetimeOperation::Release && previous <= 1))
        {
            return std::nullopt;
        }

        const std::uint32_t updated =
            operation == RhiResourceLifetimeOperation::AddReference ? previous + 1 : previous - 1;
        const std::uint32_t desiredRaw = __builtin_bswap32(updated);
        if (reference.compare_exchange_weak(expectedRaw, desiredRaw, std::memory_order_seq_cst,
                                            std::memory_order_seq_cst))
        {
            return updated;
        }
    }
}

} // namespace gears
