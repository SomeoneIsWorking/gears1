#include "titles/gears1/audio_mix.h"
#include "titles/gears1/audio_mix_differential.h"

#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string_view>
#include <vector>

#include <x360port/guest_endian.hpp>

namespace
{

using namespace gears::titles::gears1;

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "audio mix differential: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// The recorded first disagreement, which the test requires to exist.
AudioMixMismatch RequireMismatch(const AudioMixDifferential &check, std::string_view message)
{
    std::optional<AudioMixMismatch> first = check.FirstMismatch();
    if (!first)
    {
        Require(false, message);
        return {};
    }
    return *first;
}

constexpr std::uint32_t kBase = 0x40000000U;
constexpr std::uint32_t kOutput = kBase;
constexpr std::uint32_t kInput = kBase + 0x1000U;
constexpr std::uint32_t kCoefficient0 = kBase + 0x2000U;
constexpr std::uint32_t kCoefficient1 = kBase + 0x2010U;
constexpr std::uint32_t kMemoryBytes = 0x3000U;

// Flat guest memory, and an original body the test chooses.
class FakeCall final : public x360port::GuestCallContext
{
  public:
    using Original = std::function<x360port::ExecutionResult(FakeCall &, x360port::GuestAddress,
                                                             std::span<const std::uint64_t>)>;

    FakeCall() : memory_(kMemoryBytes)
    {
        for (std::uint32_t offset = 0; offset < kMemoryBytes; offset += 4U)
        {
            float sample = static_cast<float>(offset % 97U) * 0.25F - 6.0F;
            x360port::StoreGuestWord(memory_, offset, std::bit_cast<std::uint32_t>(sample));
        }
    }

    x360port::RuntimeFailure ReadMappedGuestMemory(x360port::GuestAddress address,
                                                   std::span<std::byte> bytes) const override
    {
        if (!Mapped(address, bytes.size()))
        {
            return {x360port::RuntimeError::GuestMemoryRangeInvalid, "unmapped"};
        }
        std::memcpy(bytes.data(), memory_.data() + (address - kBase), bytes.size());
        return {};
    }

    x360port::RuntimeFailure WriteMappedGuestMemory(x360port::GuestAddress address,
                                                    std::span<const std::byte> bytes) override
    {
        if (!Mapped(address, bytes.size()))
        {
            return {x360port::RuntimeError::GuestMemoryRangeInvalid, "unmapped"};
        }
        std::memcpy(memory_.data() + (address - kBase), bytes.data(), bytes.size());
        return {};
    }

    x360port::ExecutionResult CallOriginalBody(x360port::GuestAddress address,
                                               std::span<const std::uint64_t> arguments) override
    {
        ++original_calls;
        return original(*this, address, arguments);
    }

    [[nodiscard]] std::uint32_t Word(std::uint32_t address) const
    {
        return x360port::LoadGuestWord(memory_, address - kBase);
    }

    void SetWord(std::uint32_t address, std::uint32_t word)
    {
        x360port::StoreGuestWord(memory_, address - kBase, word);
    }

    [[nodiscard]] std::vector<std::byte> Snapshot() const { return memory_; }

    Original original;
    int original_calls = 0;

  private:
    [[nodiscard]] static bool Mapped(x360port::GuestAddress address, std::size_t size)
    {
        return address >= kBase && address - kBase + size <= kMemoryBytes;
    }

    std::vector<std::byte> memory_;
};

constexpr std::array<std::uint64_t, 4> kArguments{kOutput, kInput, kCoefficient0, kCoefficient1};

// An original body that agrees with the native mix.
x360port::ExecutionResult AgreeingOriginal(FakeCall &call, x360port::GuestAddress address,
                                           std::span<const std::uint64_t> arguments)
{
    return ApplyNativeAudioMix(call, address, arguments, nullptr);
}

void TestAgreementKeepsTheOriginalsSingleMix()
{
    FakeCall expected;
    x360port::ExecutionResult once =
        ApplyNativeAudioMix(expected, kAudioMixAddress, kArguments, nullptr);
    Require(static_cast<bool>(once), "the reference mix ran");

    FakeCall call;
    call.original = AgreeingOriginal;
    AudioMixDifferential check;
    x360port::ExecutionResult result =
        AudioMixDifferential::Apply(call, kAudioMixAddress, kArguments, &check);
    Require(static_cast<bool>(result), "the differential call succeeded");
    Require(result.value == once.value, "the differential returns the original's r3");
    Require(call.original_calls == 1, "the original body ran once");
    // The mix accumulates into its output, so memory equal to one mix proves
    // the native mix's writes were undone before the original ran.
    Require(call.Snapshot() == expected.Snapshot(), "guest memory holds exactly one mix");
    AudioMixDifferentialCounts counts = check.Counts();
    Require(counts.compared == 1 && counts.mismatched == 0 && counts.failed == 0,
            "one agreeing call is counted");
    Require(!check.FirstMismatch(), "agreement records no mismatch");
}

void TestAnOutputDisagreementIsLocated()
{
    FakeCall call;
    call.original =
        [](FakeCall &fake, x360port::GuestAddress address, std::span<const std::uint64_t> arguments)
    {
        x360port::ExecutionResult result = AgreeingOriginal(fake, address, arguments);
        fake.SetWord(kOutput + 0x104U, fake.Word(kOutput + 0x104U) ^ 1U);
        return result;
    };
    AudioMixDifferential check;
    Require(
        static_cast<bool>(AudioMixDifferential::Apply(call, kAudioMixAddress, kArguments, &check)),
        "a disagreeing call still returns the original's result");
    AudioMixMismatch first = RequireMismatch(check, "the disagreement is recorded");
    Require(check.Counts().mismatched == 1, "the disagreement is counted");
    Require(first.where == AudioMixMismatch::Where::Output && first.byte_offset == 0x104U,
            "the disagreement names the output word");
    Require((first.native_word ^ first.original_word) == 1U, "both words are reported");
    Require(call.Word(kOutput + 0x104U) == first.original_word,
            "guest memory keeps the original's word");
}

void TestAnInputWriteIsCaught()
{
    FakeCall call;
    call.original =
        [](FakeCall &fake, x360port::GuestAddress address, std::span<const std::uint64_t> arguments)
    {
        x360port::ExecutionResult result = AgreeingOriginal(fake, address, arguments);
        fake.SetWord(kInput + 8U, 0U);
        return result;
    };
    AudioMixDifferential check;
    (void)AudioMixDifferential::Apply(call, kAudioMixAddress, kArguments, &check);
    AudioMixMismatch first = RequireMismatch(check, "the input write is recorded");
    Require(first.where == AudioMixMismatch::Where::Input && first.byte_offset == 8U,
            "a write outside the output block is a disagreement");
}

void TestAReturnValueDisagreementIsCaught()
{
    FakeCall call;
    call.original =
        [](FakeCall &fake, x360port::GuestAddress address, std::span<const std::uint64_t> arguments)
    {
        x360port::ExecutionResult result = AgreeingOriginal(fake, address, arguments);
        result.value += 64U;
        return result;
    };
    AudioMixDifferential check;
    x360port::ExecutionResult result =
        AudioMixDifferential::Apply(call, kAudioMixAddress, kArguments, &check);
    AudioMixMismatch first = RequireMismatch(check, "the r3 difference is recorded");
    Require(first.where == AudioMixMismatch::Where::ReturnValue,
            "a different r3 is a disagreement");
    Require(result.value == first.original_return &&
                first.original_return == first.native_return + 64U,
            "the original's r3 is returned and both are reported");
}

void TestAnUnreadableBlockIsNotCompared()
{
    FakeCall call;
    call.original = AgreeingOriginal;
    AudioMixDifferential check;
    std::array<std::uint64_t, 4> unmapped{kBase + kMemoryBytes, kInput, kCoefficient0,
                                          kCoefficient1};
    (void)AudioMixDifferential::Apply(call, kAudioMixAddress, unmapped, &check);
    AudioMixDifferentialCounts counts = check.Counts();
    Require(counts.compared == 0 && counts.failed == 1 && call.original_calls == 1,
            "an unreadable block runs only the original and counts a failure");
}

void TestFirstDifferingWord()
{
    std::array<std::byte, 16> left{};
    std::array<std::byte, 16> right{};
    Require(!FirstDifferingWord(left, right), "identical blocks have no difference");
    right[13] = std::byte{1};
    Require(FirstDifferingWord(left, right) == 12U, "the differing word is found by offset");
}

} // namespace

int main()
{
    TestAgreementKeepsTheOriginalsSingleMix();
    TestAnOutputDisagreementIsLocated();
    TestAnInputWriteIsCaught();
    TestAReturnValueDisagreementIsCaught();
    TestAnUnreadableBlockIsNotCompared();
    TestFirstDifferingWord();
    std::cout << "audio mix differential: 6 checks passed\n";
    return EXIT_SUCCESS;
}
