#include "audio_mix.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <x360port/guest_endian.hpp>

namespace gears::titles::gears1
{
namespace
{

using GuestVector = std::array<float, 4>;

constexpr std::size_t kGuestVectorSize = 16U;
constexpr std::uint32_t kGuestVectorMask = ~std::uint32_t{0x0FU};
// Sixteen iterations of four vectors each, for both the input and the output.
constexpr std::uint32_t kMixIterations = 16U;
constexpr std::uint32_t kMixBlockBytes = kMixIterations * 4U * kGuestVectorSize;

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

[[nodiscard]] GuestVector DecodeGuestVector(std::span<const std::byte> bytes) noexcept
{
    GuestVector value{};
    for (std::size_t lane = 0; lane < value.size(); ++lane)
    {
        const std::size_t guestLane = value.size() - lane - 1U;
        value[lane] = std::bit_cast<float>(x360port::LoadGuestWord(bytes, guestLane * 4U));
    }
    return value;
}

void EncodeGuestVector(std::span<std::byte> bytes, const GuestVector &value) noexcept
{
    for (std::size_t lane = 0; lane < value.size(); ++lane)
    {
        const std::size_t guestLane = value.size() - lane - 1U;
        x360port::StoreGuestWord(bytes, guestLane * 4U, std::bit_cast<std::uint32_t>(value[lane]));
    }
}

[[nodiscard]] bool LoadGuestVector(const x360port::GuestCallContext &call, std::uint32_t address,
                                   GuestVector &value, x360port::RuntimeFailure &failure) noexcept
{
    std::array<std::byte, kGuestVectorSize> bytes{};
    failure = call.ReadMappedGuestMemory(address & kGuestVectorMask, bytes);
    if (failure)
    {
        return false;
    }
    value = DecodeGuestVector(bytes);
    return true;
}

// The input and output blocks the mix reads and writes, staged in host memory:
// one validated guest read per block before the kernel runs and one validated
// write of the output block after it, instead of a validated access for every
// vector. Blocks that overlap share one staged range, so a store is seen by
// every later load of the same bytes exactly as on the guest.
class StagedMixBlocks final
{
  public:
    enum class Block : std::uint8_t
    {
        Input,
        Output,
    };

    [[nodiscard]] x360port::RuntimeFailure Stage(const x360port::GuestCallContext &call,
                                                 std::uint32_t input, std::uint32_t output) noexcept
    {
        output_ = output & kGuestVectorMask;
        const std::uint32_t inputBase = input & kGuestVectorMask;
        const bool overlap =
            output_ - inputBase < kMixBlockBytes || inputBase - output_ < kMixBlockBytes;
        if (!overlap)
        {
            inputStart_ = 0;
            outputStart_ = kMixBlockBytes;
            x360port::RuntimeFailure failure = call.ReadMappedGuestMemory(
                inputBase, std::span(storage_).subspan(inputStart_, kMixBlockBytes));
            if (failure)
            {
                return failure;
            }
            return call.ReadMappedGuestMemory(
                output_, std::span(storage_).subspan(outputStart_, kMixBlockBytes));
        }
        const std::uint32_t base = inputBase < output_ ? inputBase : output_;
        inputStart_ = inputBase - base;
        outputStart_ = output_ - base;
        const std::size_t size =
            (inputStart_ > outputStart_ ? inputStart_ : outputStart_) + kMixBlockBytes;
        return call.ReadMappedGuestMemory(base, std::span(storage_).first(size));
    }

    [[nodiscard]] GuestVector Load(Block block, std::uint32_t offset) const noexcept
    {
        return DecodeGuestVector(
            std::span(storage_).subspan(Start(block) + offset, kGuestVectorSize));
    }

    void Store(Block block, std::uint32_t offset, const GuestVector &value) noexcept
    {
        EncodeGuestVector(std::span(storage_).subspan(Start(block) + offset, kGuestVectorSize),
                          value);
    }

    // The mix stores only into the output block.
    [[nodiscard]] x360port::RuntimeFailure Commit(x360port::GuestCallContext &call) const noexcept
    {
        return call.WriteMappedGuestMemory(
            output_, std::span(storage_).subspan(outputStart_, kMixBlockBytes));
    }

  private:
    [[nodiscard]] std::size_t Start(Block block) const noexcept
    {
        return block == Block::Input ? inputStart_ : outputStart_;
    }

    std::array<std::byte, 2U * kMixBlockBytes> storage_{};
    std::uint32_t output_ = 0;
    std::size_t inputStart_ = 0;
    std::size_t outputStart_ = 0;
};

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

    const std::uint32_t output = static_cast<std::uint32_t>(arguments[0]);
    const std::uint32_t input = static_cast<std::uint32_t>(arguments[1]);
    const std::uint32_t coefficient0 = static_cast<std::uint32_t>(arguments[2]);
    const std::uint32_t coefficient1 = static_cast<std::uint32_t>(arguments[3]);

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

    StagedMixBlocks blocks;
    failure = blocks.Stage(call, input, output);
    if (failure)
    {
        return Failed(std::move(failure));
    }
    using Block = StagedMixBlocks::Block;
    for (std::uint32_t iteration = 0; iteration < kMixIterations; ++iteration)
    {
        const std::uint32_t block = iteration * 64U;
        lastInputBlock = input + block;

        GuestVector v9 = blocks.Load(Block::Output, block + 48U);
        GuestVector v7 = blocks.Load(Block::Input, block + 48U);
        const GuestVector v4 = blocks.Load(Block::Output, block);
        v9 = MultiplyAdd(v7, v10, v9);

        GuestVector v6 = blocks.Load(Block::Input, block);
        GuestVector v8 = blocks.Load(Block::Input, block + 32U);
        v7 = MultiplyAdd(v6, v13, v4);

        const GuestVector v3 = blocks.Load(Block::Output, block + 16U);
        const GuestVector v2 = blocks.Load(Block::Output, block + 32U);
        v13 = Add(v13, v0);

        const GuestVector v5 = blocks.Load(Block::Input, block + 16U);
        v8 = MultiplyAdd(v8, v11, v2);
        v6 = MultiplyAdd(v5, v12, v3);
        v12 = Add(v12, v0);
        v11 = Add(v11, v0);
        v10 = Add(v10, v0);

        blocks.Store(Block::Output, block + 48U, v9);
        blocks.Store(Block::Output, block, v7);
        blocks.Store(Block::Output, block + 32U, v8);
        blocks.Store(Block::Output, block + 16U, v6);
    }
    failure = blocks.Commit(call);
    if (failure)
    {
        return Failed(std::move(failure));
    }

    return {{}, lastInputBlock};
}

} // namespace gears::titles::gears1
