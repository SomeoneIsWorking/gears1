#include "audio_mix_differential.h"

#include <x360port/guest_endian.hpp>

#include "audio_mix.h"

namespace gears::titles::gears1
{
namespace
{

// The mix addresses whole vectors, so its blocks start on a vector boundary.
inline constexpr std::uint32_t kVectorMask = ~std::uint32_t{0x0FU};

[[nodiscard]] std::uint32_t BlockBase(std::uint64_t pointer) noexcept
{
    return static_cast<std::uint32_t>(pointer) & kVectorMask;
}

} // namespace

std::optional<std::uint32_t> FirstDifferingWord(std::span<const std::byte> left,
                                                std::span<const std::byte> right)
{
    for (std::size_t offset = 0; offset + 4U <= left.size(); offset += 4U)
    {
        if (x360port::LoadGuestWord(left, offset) != x360port::LoadGuestWord(right, offset))
        {
            return static_cast<std::uint32_t>(offset);
        }
    }
    return std::nullopt;
}

x360port::ExecutionResult AudioMixDifferential::Apply(x360port::GuestCallContext &call,
                                                      x360port::GuestAddress address,
                                                      std::span<const std::uint64_t> arguments,
                                                      void *context) noexcept
{
    return static_cast<AudioMixDifferential *>(context)->Compare(call, address, arguments);
}

x360port::ExecutionResult AudioMixDifferential::Compare(x360port::GuestCallContext &call,
                                                        x360port::GuestAddress address,
                                                        std::span<const std::uint64_t> arguments)
{
    if (arguments.size() < 4U)
    {
        return ApplyNativeAudioMix(call, address, arguments, nullptr);
    }
    std::uint32_t output = BlockBase(arguments[0]);
    std::uint32_t input = BlockBase(arguments[1]);

    Blocks before;
    if (call.ReadMappedGuestMemory(input, before.input) ||
        call.ReadMappedGuestMemory(output, before.output))
    {
        failed_.fetch_add(1, std::memory_order_relaxed);
        return call.CallOriginalBody(address, arguments);
    }

    x360port::ExecutionResult native = ApplyNativeAudioMix(call, address, arguments, nullptr);
    Blocks nativeAfter;
    bool nativeRead = native && !call.ReadMappedGuestMemory(input, nativeAfter.input) &&
                      !call.ReadMappedGuestMemory(output, nativeAfter.output);
    // Restore in capture order so overlapping blocks end as they began.
    if (call.WriteMappedGuestMemory(input, before.input) ||
        call.WriteMappedGuestMemory(output, before.output))
    {
        failed_.fetch_add(1, std::memory_order_relaxed);
        return {{x360port::RuntimeError::ExecutionFailed,
                 "Gears audio mix differential could not restore the guest blocks"},
                0};
    }

    x360port::ExecutionResult original = call.CallOriginalBody(address, arguments);
    Blocks originalAfter;
    if (!nativeRead || !original || call.ReadMappedGuestMemory(input, originalAfter.input) ||
        call.ReadMappedGuestMemory(output, originalAfter.output))
    {
        failed_.fetch_add(1, std::memory_order_relaxed);
        return original;
    }

    compared_.fetch_add(1, std::memory_order_relaxed);
    AudioMixMismatch mismatch{.output = output,
                              .input = input,
                              .native_return = native.value,
                              .original_return = original.value};
    std::optional<std::uint32_t> offset =
        FirstDifferingWord(nativeAfter.output, originalAfter.output);
    mismatch.where = AudioMixMismatch::Where::Output;
    std::span<const std::byte> nativeBlock = nativeAfter.output;
    std::span<const std::byte> originalBlock = originalAfter.output;
    if (!offset)
    {
        offset = FirstDifferingWord(nativeAfter.input, originalAfter.input);
        mismatch.where = AudioMixMismatch::Where::Input;
        nativeBlock = nativeAfter.input;
        originalBlock = originalAfter.input;
    }
    if (offset)
    {
        mismatch.byte_offset = *offset;
        mismatch.native_word = x360port::LoadGuestWord(nativeBlock, *offset);
        mismatch.original_word = x360port::LoadGuestWord(originalBlock, *offset);
        Record(mismatch);
    }
    else if (native.value != original.value)
    {
        mismatch.where = AudioMixMismatch::Where::ReturnValue;
        Record(mismatch);
    }
    return original;
}

void AudioMixDifferential::Record(const AudioMixMismatch &mismatch)
{
    mismatched_.fetch_add(1, std::memory_order_relaxed);
    std::scoped_lock lock(first_mismatch_mutex_);
    if (!first_mismatch_)
    {
        first_mismatch_ = mismatch;
    }
}

AudioMixDifferentialCounts AudioMixDifferential::Counts() const noexcept
{
    return {.compared = compared_.load(std::memory_order_relaxed),
            .mismatched = mismatched_.load(std::memory_order_relaxed),
            .failed = failed_.load(std::memory_order_relaxed)};
}

std::optional<AudioMixMismatch> AudioMixDifferential::FirstMismatch() const
{
    std::scoped_lock lock(first_mismatch_mutex_);
    return first_mismatch_;
}

} // namespace gears::titles::gears1
