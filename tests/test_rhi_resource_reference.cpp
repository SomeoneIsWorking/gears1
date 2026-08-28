#include "guest_state_memory.h"
#include "rhi_resource_reference.h"

#include <cassert>
#include <cstdint>
#include <thread>
#include <vector>

int main()
{
    constexpr std::uint32_t kObject = 0x40;
    constexpr std::uint32_t kReferenceCountOffset = 4;
    std::vector<std::uint8_t> guest(0x100);
    gears::titles::gears1::GuestStateMemory memory(guest.data());

    memory.Write32(kObject + kReferenceCountOffset, 2);
    auto result = gears::TryApplyNativeRhiReferenceFastPath(
        guest.data(), kObject, gears::RhiResourceLifetimeOperation::AddReference);
    assert(result == 3);
    assert(memory.Read32(kObject + kReferenceCountOffset) == 3);

    result = gears::TryApplyNativeRhiReferenceFastPath(
        guest.data(), kObject, gears::RhiResourceLifetimeOperation::Release);
    assert(result == 2);
    assert(memory.Read32(kObject + kReferenceCountOffset) == 2);

    memory.Write32(kObject + kReferenceCountOffset, 0);
    result = gears::TryApplyNativeRhiReferenceFastPath(
        guest.data(), kObject, gears::RhiResourceLifetimeOperation::AddReference);
    assert(!result.has_value());
    assert(memory.Read32(kObject + kReferenceCountOffset) == 0);

    memory.Write32(kObject + kReferenceCountOffset, 1);
    result = gears::TryApplyNativeRhiReferenceFastPath(
        guest.data(), kObject, gears::RhiResourceLifetimeOperation::Release);
    assert(!result.has_value());
    assert(memory.Read32(kObject + kReferenceCountOffset) == 1);

    memory.Write32(kObject + kReferenceCountOffset, 2);
    constexpr std::uint32_t kThreadCount = 4;
    constexpr std::uint32_t kCallsPerThread = 10'000;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::uint32_t thread = 0; thread < kThreadCount; ++thread)
    {
        threads.emplace_back(
            [&guest]()
            {
                for (std::uint32_t call = 0; call < kCallsPerThread; ++call)
                {
                    const auto updated = gears::TryApplyNativeRhiReferenceFastPath(
                        guest.data(), kObject, gears::RhiResourceLifetimeOperation::AddReference);
                    assert(updated.has_value());
                }
            });
    }
    for (std::thread &thread : threads)
        thread.join();
    assert(memory.Read32(kObject + kReferenceCountOffset) == 2 + kThreadCount * kCallsPerThread);

    return 0;
}
