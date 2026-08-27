#include "titles/gears1/color_write_gamma_state.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

using gears::titles::gears1::ApplyNativeColorWriteGammaState;
using namespace gears::titles::gears1::color_write_gamma;

class TestMemory
{
  public:
    explicit TestMemory(std::size_t size) : bytes_(size) {}

    [[nodiscard]] std::uint8_t Read8(std::uint32_t address) const { return bytes_.at(address); }

    [[nodiscard]] std::uint32_t Read32(std::uint32_t address) const
    {
        return (std::uint32_t{Read8(address)} << 24) | (std::uint32_t{Read8(address + 1)} << 16) |
               (std::uint32_t{Read8(address + 2)} << 8) | Read8(address + 3);
    }

    [[nodiscard]] std::uint64_t Read64(std::uint32_t address) const
    {
        return (std::uint64_t{Read32(address)} << 32) | Read32(address + 4);
    }

    void Write8(std::uint32_t address, std::uint8_t value) { bytes_.at(address) = value; }

    void Write32(std::uint32_t address, std::uint32_t value)
    {
        Write8(address, static_cast<std::uint8_t>(value >> 24));
        Write8(address + 1, static_cast<std::uint8_t>(value >> 16));
        Write8(address + 2, static_cast<std::uint8_t>(value >> 8));
        Write8(address + 3, static_cast<std::uint8_t>(value));
    }

    void Write64(std::uint32_t address, std::uint64_t value)
    {
        Write32(address, static_cast<std::uint32_t>(value >> 32));
        Write32(address + 4, static_cast<std::uint32_t>(value));
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

constexpr std::uint32_t kDevice = 0x1000;
constexpr std::uint32_t kTarget = 0x5000;

void PrepareTarget(TestMemory &memory, std::uint32_t format)
{
    memory.Write32(kDevice + kColorTargetObjectOffset, kTarget);
    memory.Write32(kTarget + kObjectDescriptorOffset, 0xA5A0005Au | (format << 16));
    memory.Write32(kDevice + kColorDescriptorOffset, 0x5A5000A5u | (format << 16));
    memory.Write64(kDevice + kDirtyMaskOffset, 0x1234);
}

void ExpectTransition(std::uint32_t before, std::uint32_t requested, std::uint32_t after)
{
    TestMemory memory(0x10000);
    PrepareTarget(memory, before);

    assert(ApplyNativeColorWriteGammaState(memory, kDevice, requested));
    assert(memory.Read32(kDevice + kRequestedStateOffset) == requested);
    assert(((memory.Read32(kTarget + kObjectDescriptorOffset) >> 16) & 0xF) == after);
    assert(((memory.Read32(kDevice + kColorDescriptorOffset) >> 16) & 0xF) == after);
    assert(memory.Read64(kDevice + kDirtyMaskOffset) == (0x1234 | kDirtyMask));
}

void TestFormatPairTransitions()
{
    ExpectTransition(2, 1, 10);
    ExpectTransition(10, 0, 2);
    ExpectTransition(3, 1, 12);
    ExpectTransition(12, 0, 3);
}

void TestMatchingAndUnsupportedFormatsDoNotDirty()
{
    for (const auto [format, requested] : {std::pair{2u, 0u}, std::pair{10u, 1u}, std::pair{3u, 0u},
                                           std::pair{12u, 1u}, std::pair{7u, 1u}})
    {
        TestMemory memory(0x10000);
        PrepareTarget(memory, format);
        const std::uint32_t objectDescriptor = memory.Read32(kTarget + kObjectDescriptorOffset);
        const std::uint32_t deviceDescriptor = memory.Read32(kDevice + kColorDescriptorOffset);

        assert(!ApplyNativeColorWriteGammaState(memory, kDevice, requested));
        assert(memory.Read32(kDevice + kRequestedStateOffset) == requested);
        assert(memory.Read32(kTarget + kObjectDescriptorOffset) == objectDescriptor);
        assert(memory.Read32(kDevice + kColorDescriptorOffset) == deviceDescriptor);
        assert(memory.Read64(kDevice + kDirtyMaskOffset) == 0x1234);
    }
}

void TestNoBoundTargetStillStoresRequestedState()
{
    TestMemory memory(0x10000);
    assert(!ApplyNativeColorWriteGammaState(memory, kDevice, 1));
    assert(memory.Read32(kDevice + kRequestedStateOffset) == 1);
}

void TestNonBooleanInputPreservesRetainedArithmetic()
{
    TestMemory memory(0x10000);
    PrepareTarget(memory, 2);
    assert(ApplyNativeColorWriteGammaState(memory, kDevice, 2));
    assert(((memory.Read32(kTarget + kObjectDescriptorOffset) >> 16) & 0xF) == 10);
    assert(memory.Read64(kDevice + kDirtyMaskOffset) == (0x1234 | kDirtyMask));
}

} // namespace

int main()
{
    TestFormatPairTransitions();
    TestMatchingAndUnsupportedFormatsDoNotDirty();
    TestNoBoundTargetStillStoresRequestedState();
    TestNonBooleanInputPreservesRetainedArithmetic();
}
