#include "audio_mix.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace gears::titles::gears1
{
namespace
{

using GuestVector = std::array<float, 4>;

constexpr std::size_t kGuestVectorSize = 16U;
constexpr std::uint32_t kGuestVectorMask = ~std::uint32_t{0x0FU};

[[nodiscard]] std::uint32_t LoadBigEndian(std::span<const std::byte> bytes,
                                          std::size_t offset) noexcept
{
    return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) << 24U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 8U) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U]));
}

void StoreBigEndian(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) noexcept
{
    bytes[offset] = static_cast<std::byte>(value >> 24U);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 16U);
    bytes[offset + 2U] = static_cast<std::byte>(value >> 8U);
    bytes[offset + 3U] = static_cast<std::byte>(value);
}

[[nodiscard]] GuestVector Add(const GuestVector &left, const GuestVector &right) noexcept
{
    GuestVector result{};
    for (std::size_t lane = 0; lane < result.size(); ++lane)
    {
        result[lane] = left[lane] + right[lane];
    }
    return result;
}

// The guest VMX `vmaddfp` flushes denormal inputs to zero with their sign
// preserved, regardless of the non-Java mode bit, and rounds the product and
// sum only once.
[[nodiscard]] float FlushDenormal(float value) noexcept
{
    if (std::fpclassify(value) == FP_SUBNORMAL)
    {
        return std::copysign(0.0F, value);
    }
    return value;
}

// (VD) <- ((VA) * (VC)) + (VB), as one fused operation per lane.
[[nodiscard]] GuestVector MultiplyAdd(const GuestVector &va, const GuestVector &vc,
                                      const GuestVector &vb) noexcept
{
    GuestVector result{};
    for (std::size_t lane = 0; lane < result.size(); ++lane)
    {
        result[lane] =
            std::fma(FlushDenormal(va[lane]), FlushDenormal(vc[lane]), FlushDenormal(vb[lane]));
    }
    return result;
}

[[nodiscard]] bool LoadGuestVector(x360port::GuestCallContext &call, std::uint32_t address,
                                   GuestVector &value, x360port::RuntimeFailure &failure) noexcept
{
    std::array<std::byte, kGuestVectorSize> bytes{};
    failure = call.ReadMappedGuestMemory(address & kGuestVectorMask, bytes);
    if (failure)
    {
        return false;
    }
    for (std::size_t lane = 0; lane < value.size(); ++lane)
    {
        const std::size_t guestLane = value.size() - lane - 1U;
        value[lane] = std::bit_cast<float>(LoadBigEndian(bytes, guestLane * 4U));
    }
    return true;
}

[[nodiscard]] bool StoreGuestVector(x360port::GuestCallContext &call, std::uint32_t address,
                                    const GuestVector &value,
                                    x360port::RuntimeFailure &failure) noexcept
{
    std::array<std::byte, kGuestVectorSize> bytes{};
    failure = call.ReadMappedGuestMemory(address & kGuestVectorMask, bytes);
    if (failure)
    {
        return false;
    }
    for (std::size_t lane = 0; lane < value.size(); ++lane)
    {
        const std::size_t guestLane = value.size() - lane - 1U;
        StoreBigEndian(bytes, guestLane * 4U, std::bit_cast<std::uint32_t>(value[lane]));
    }
    failure = call.WriteMappedGuestMemory(address & kGuestVectorMask, bytes);
    return !failure;
}

[[nodiscard]] x360port::ExecutionResult Failed(x360port::RuntimeFailure failure) noexcept
{
    return {std::move(failure), 0};
}

} // namespace

x360port::ExecutionResult ApplyNativeAudioMix(x360port::GuestCallContext &call,
                                              x360port::GuestAddress,
                                              std::span<const std::uint64_t> arguments,
                                              void *) noexcept
{
    if (arguments.size() < 4U)
    {
        return {{x360port::RuntimeError::ExecutionFailed,
                 "Gears audio mix override requires four guest pointer arguments"},
                0};
    }

    std::uint32_t output = static_cast<std::uint32_t>(arguments[0]);
    const std::uint32_t input = static_cast<std::uint32_t>(arguments[1]);
    const std::uint32_t coefficient0 = static_cast<std::uint32_t>(arguments[2]);
    const std::uint32_t coefficient1 = static_cast<std::uint32_t>(arguments[3]);
    const std::uint32_t outputInputDelta = output - input;
    std::uint32_t cursor = input + 32U;

    // The guest leaves the last processed input block in r3 and returns it
    // without setting a result, so the override reproduces that exact value.
    std::uint32_t lastInputBlock = input;

    x360port::RuntimeFailure failure;
    GuestVector v0{};
    if (!LoadGuestVector(call, coefficient1, v0, failure))
    {
        return Failed(std::move(failure));
    }
    GuestVector v11 = Add(v0, v0);
    GuestVector v13{};
    if (!LoadGuestVector(call, coefficient0, v13, failure))
    {
        return Failed(std::move(failure));
    }
    GuestVector v12 = Add(v13, v0);
    GuestVector v10 = Add(v11, v0);
    v0 = Add(v11, v11);
    v11 = Add(v13, v11);
    v10 = Add(v13, v10);

    for (std::uint32_t iteration = 0; iteration < 16U; ++iteration)
    {
        const std::uint32_t output0 = output + 48U;
        const std::uint32_t output1 = output;
        const std::uint32_t output2 = output + 16U;
        const std::uint32_t input0 = cursor + 16U;
        const std::uint32_t input1 = cursor - 32U;
        lastInputBlock = input1;
        const std::uint32_t input2 = outputInputDelta + cursor;
        const std::uint32_t input3 = cursor - 16U;

        GuestVector v9{};
        if (!LoadGuestVector(call, output0, v9, failure))
        {
            return Failed(std::move(failure));
        }
        GuestVector v7{};
        if (!LoadGuestVector(call, input0, v7, failure))
        {
            return Failed(std::move(failure));
        }
        GuestVector v4{};
        if (!LoadGuestVector(call, output1, v4, failure))
        {
            return Failed(std::move(failure));
        }
        v9 = MultiplyAdd(v7, v10, v9);

        GuestVector v6{};
        if (!LoadGuestVector(call, input1, v6, failure))
        {
            return Failed(std::move(failure));
        }
        GuestVector v8{};
        if (!LoadGuestVector(call, cursor, v8, failure))
        {
            return Failed(std::move(failure));
        }
        v7 = MultiplyAdd(v6, v13, v4);

        GuestVector v3{};
        if (!LoadGuestVector(call, output2, v3, failure))
        {
            return Failed(std::move(failure));
        }
        cursor += 64U;

        GuestVector v2{};
        if (!LoadGuestVector(call, input2, v2, failure))
        {
            return Failed(std::move(failure));
        }
        v13 = Add(v13, v0);

        GuestVector v5{};
        if (!LoadGuestVector(call, input3, v5, failure))
        {
            return Failed(std::move(failure));
        }
        v8 = MultiplyAdd(v8, v11, v2);
        v6 = MultiplyAdd(v5, v12, v3);
        v12 = Add(v12, v0);
        v11 = Add(v11, v0);
        v10 = Add(v10, v0);

        if (!StoreGuestVector(call, output0, v9, failure) ||
            !StoreGuestVector(call, output1, v7, failure) ||
            !StoreGuestVector(call, input2, v8, failure) ||
            !StoreGuestVector(call, output2, v6, failure))
        {
            return Failed(std::move(failure));
        }
        output += 64U;
    }

    return {{}, lastInputBlock};
}

} // namespace gears::titles::gears1
