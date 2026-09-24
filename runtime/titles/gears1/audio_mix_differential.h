#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

#include <x360port/guest_call.hpp>

namespace gears::titles::gears1
{

// The guest blocks one mix reads and writes: sixteen iterations of four
// sixteen-byte vectors.
inline constexpr std::uint32_t kAudioMixBlockBytes = 1024U;

// The first call on which the native mix and the original guest body disagreed.
struct AudioMixMismatch
{
    std::uint32_t output = 0;
    std::uint32_t input = 0;
    // Which block differed first: the output, the input, or neither (r3 only).
    enum class Where : std::uint8_t
    {
        Output,
        Input,
        ReturnValue,
    } where = Where::ReturnValue;
    std::uint32_t byte_offset = 0;
    std::uint32_t native_word = 0;
    std::uint32_t original_word = 0;
    std::uint64_t native_return = 0;
    std::uint64_t original_return = 0;
};

struct AudioMixDifferentialCounts
{
    std::uint64_t compared = 0;
    std::uint64_t mismatched = 0;
    // Calls where either side, or restoring the snapshot, failed; not compared.
    std::uint64_t failed = 0;
};

// Checks the native audio mix against the guest's own body on live inputs. On
// every call it snapshots the input and output blocks, runs the native mix,
// restores the snapshot, and runs the original body through Xenia, then
// compares both blocks and r3. The original's result stays in guest memory
// and is returned, so the title plays exactly as without the override. A
// maintainer mode: every call pays for both implementations.
class AudioMixDifferential final
{
  public:
    // A NativeOverrideHandler whose context is the AudioMixDifferential.
    [[nodiscard]] static x360port::ExecutionResult Apply(x360port::GuestCallContext &call,
                                                         x360port::GuestAddress address,
                                                         std::span<const std::uint64_t> arguments,
                                                         void *context) noexcept;

    [[nodiscard]] AudioMixDifferentialCounts Counts() const noexcept;
    [[nodiscard]] std::optional<AudioMixMismatch> FirstMismatch() const;

  private:
    using Block = std::array<std::byte, kAudioMixBlockBytes>;

    struct Blocks
    {
        Block input{};
        Block output{};
    };

    [[nodiscard]] x360port::ExecutionResult Compare(x360port::GuestCallContext &call,
                                                    x360port::GuestAddress address,
                                                    std::span<const std::uint64_t> arguments);
    void Record(const AudioMixMismatch &mismatch);

    std::atomic<std::uint64_t> compared_{0};
    std::atomic<std::uint64_t> mismatched_{0};
    std::atomic<std::uint64_t> failed_{0};
    mutable std::mutex first_mismatch_mutex_;
    std::optional<AudioMixMismatch> first_mismatch_;
};

// The first differing big-endian word of two equal-length blocks, as its byte
// offset; nullopt when they are identical.
[[nodiscard]] std::optional<std::uint32_t> FirstDifferingWord(std::span<const std::byte> left,
                                                              std::span<const std::byte> right);

} // namespace gears::titles::gears1
