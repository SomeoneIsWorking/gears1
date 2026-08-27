#include "titles/gears1/shader_setter_state.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

using gears::titles::gears1::ApplyNativeShaderSetter;
using gears::titles::gears1::CanApplyNativeShaderSetter;
using gears::titles::gears1::FirstShaderSetterStateMismatch;
using gears::titles::gears1::ShaderStage;

class TestMemory
{
  public:
    explicit TestMemory(std::size_t size) : bytes_(size) {}

    [[nodiscard]] std::uint8_t Read8(std::uint32_t address) const { return bytes_.at(address); }

    [[nodiscard]] std::uint16_t Read16(std::uint32_t address) const
    {
        return (std::uint16_t{Read8(address)} << 8) | Read8(address + 1);
    }

    [[nodiscard]] std::uint32_t Read32(std::uint32_t address) const
    {
        return (std::uint32_t{Read16(address)} << 16) | Read16(address + 2);
    }

    [[nodiscard]] std::uint64_t Read64(std::uint32_t address) const
    {
        return (std::uint64_t{Read32(address)} << 32) | Read32(address + 4);
    }

    void Write8(std::uint32_t address, std::uint8_t value) { bytes_.at(address) = value; }

    void Write16(std::uint32_t address, std::uint16_t value)
    {
        Write8(address, static_cast<std::uint8_t>(value >> 8));
        Write8(address + 1, static_cast<std::uint8_t>(value));
    }

    void Write32(std::uint32_t address, std::uint32_t value)
    {
        Write16(address, static_cast<std::uint16_t>(value >> 16));
        Write16(address + 2, static_cast<std::uint16_t>(value));
    }

    void Write64(std::uint32_t address, std::uint64_t value)
    {
        Write32(address, static_cast<std::uint32_t>(value >> 32));
        Write32(address + 4, static_cast<std::uint32_t>(value));
    }

    void CopyBytesForward(std::uint32_t destination, std::uint32_t source, std::uint32_t bytes)
    {
        for (std::uint32_t byte = 0; byte < bytes; ++byte)
            Write8(destination + byte, Read8(source + byte));
    }

  private:
    std::vector<std::uint8_t> bytes_;
};

constexpr std::uint32_t kDevice = 0x1000;
constexpr std::uint32_t kShader = 0x8000;

std::uint32_t BuildPixelPatch(TestMemory &memory)
{
    constexpr std::uint32_t kHeader = kShader + 0x28;
    constexpr std::uint32_t kPatch = kHeader + 0x100;
    constexpr std::uint32_t kRecords = kPatch + 0x14;

    memory.Write32(kHeader + 0x14, 0x100);
    memory.Write64(kPatch, 0xF0);
    memory.Write64(kPatch + 8, 1);

    std::uint32_t cursor = kRecords;
    memory.Write16(cursor, 0);
    memory.Write16(cursor + 2, 1);
    memory.Write32(cursor + 4, 0xDEADBEEF);
    cursor += 8;
    memory.Write32(cursor, 0);
    cursor += 4;

    memory.Write16(cursor, 0x20);
    memory.Write16(cursor + 2, 2);
    memory.Write32(cursor + 4, 0x11223344);
    memory.Write32(cursor + 8, 0x55667788);
    cursor += 12;
    memory.Write32(cursor, 0);
    cursor += 4;

    memory.Write16(cursor, 0x40);
    memory.Write16(cursor + 2, 2);
    memory.Write32(cursor + 4, 0xFFFF00FF);
    memory.Write32(cursor + 8, 0x00005500);
    cursor += 12;
    memory.Write32(cursor, 0);
    cursor += 4;

    memory.Write32(kPatch + 0x10, cursor - kRecords);
    return kPatch;
}

void TestPixelShaderStateAndPatchRecords()
{
    TestMemory memory(0x20000);
    BuildPixelPatch(memory);
    memory.Write64(kDevice + 8, 0xFFFF);
    memory.Write64(kDevice + 0x18, 8);
    memory.Write32(kDevice + 0x400 + 0x40, 0x1234AA78);

    assert(CanApplyNativeShaderSetter(memory, ShaderStage::Pixel, kDevice, kShader));
    ApplyNativeShaderSetter(memory, ShaderStage::Pixel, kDevice, kShader);

    assert(memory.Read32(kDevice + 0x3080) == kShader);
    assert(memory.Read64(kDevice + 0x10) == ((std::uint64_t{1} << 52) | (std::uint64_t{1} << 49)));
    assert(memory.Read64(kDevice + 8) == 0xFF0F);
    assert(memory.Read64(kDevice + 0x18) == 10);
    assert(memory.Read32(kDevice + 0x400 + 0x20) == 0x11223344);
    assert(memory.Read32(kDevice + 0x400 + 0x24) == 0x55667788);
    assert(memory.Read32(kDevice + 0x400 + 0x40) == 0x12345578);
}

void TestVertexNullShader()
{
    TestMemory memory(0x20000);
    memory.Write64(kDevice + 0x10, 7);
    memory.Write8(kDevice + 0x2A3A, 0xFF);

    assert(CanApplyNativeShaderSetter(memory, ShaderStage::Vertex, kDevice, 0));
    ApplyNativeShaderSetter(memory, ShaderStage::Vertex, kDevice, 0);

    assert(memory.Read32(kDevice + 0x3084) == 0);
    assert(memory.Read64(kDevice + 0x10) == 7);
    assert(memory.Read8(kDevice + 0x2A3A) == 0x7F);
}

void TestPreviousShaderFenceStamp()
{
    constexpr std::uint32_t kPrevious = 0xA000;
    TestMemory memory(0x20000);
    memory.Write32(kDevice + 0x3080, kPrevious);
    memory.Write32(kDevice + 0x2A1C, 77);

    assert(CanApplyNativeShaderSetter(memory, ShaderStage::Pixel, kDevice, 0));
    ApplyNativeShaderSetter(memory, ShaderStage::Pixel, kDevice, 0);
    assert(memory.Read32(kPrevious + 8) == 77);
}

void TestDeferredReleaseUsesRetainedBody()
{
    constexpr std::uint32_t kPrevious = 0xA000;
    TestMemory memory(0x20000);
    memory.Write32(kDevice + 0x3084, kPrevious);
    memory.Write32(kDevice + 0x2A20, 4);
    memory.Write32(kPrevious, 4);

    assert(!CanApplyNativeShaderSetter(memory, ShaderStage::Vertex, kDevice, kShader));
}

void TestMalformedPatchUsesRetainedBody()
{
    TestMemory memory(0x20000);
    constexpr std::uint32_t kHeader = kShader + 0x28;
    constexpr std::uint32_t kPatch = kHeader + 0x100;
    constexpr std::uint32_t kRecords = kPatch + 0x14;
    memory.Write32(kHeader + 0x14, 0x100);
    memory.Write32(kPatch + 0x10, 16);
    memory.Write32(kRecords, 0);
    memory.Write32(kRecords + 4, 0);
    memory.Write16(kRecords + 8, 0);
    memory.Write16(kRecords + 10, 1);
    memory.Write32(kRecords + 12, 0xFFFFFFFF);

    assert(!CanApplyNativeShaderSetter(memory, ShaderStage::Pixel, kDevice, kShader));
}

void TestPatchCopyPreservesGuestForwardOverlap()
{
    TestMemory memory(0x20000);
    constexpr std::uint32_t kHeader = kShader + 0x28;
    constexpr std::uint32_t kPatch = kHeader + 0x100;
    constexpr std::uint32_t kRecords = kPatch + 0x14;
    constexpr std::uint32_t kCopySource = kRecords + 8;
    constexpr std::uint16_t kDestinationOffset =
        static_cast<std::uint16_t>(kCopySource + 1 - (kDevice + 0x400));

    memory.Write32(kHeader + 0x14, 0x100);
    memory.Write32(kPatch + 0x10, 16);
    memory.Write32(kRecords, 0);
    memory.Write16(kRecords + 4, kDestinationOffset);
    memory.Write16(kRecords + 6, 2);
    memory.Write32(kCopySource, 0x11223344);
    memory.Write32(kCopySource + 4, 0x55667788);

    assert(CanApplyNativeShaderSetter(memory, ShaderStage::Pixel, kDevice, kShader));
    ApplyNativeShaderSetter(memory, ShaderStage::Pixel, kDevice, kShader);
    for (std::uint32_t byte = 1; byte <= 8; ++byte)
        assert(memory.Read8(kCopySource + byte) == 0x11);
}

void TestAuditComparatorNegativeControl()
{
    std::array<std::uint8_t, 4> expected{1, 2, 3, 4};
    std::array<std::uint8_t, 4> actual = expected;
    assert(!FirstShaderSetterStateMismatch(expected, actual));
    actual[2] ^= 1;
    assert(FirstShaderSetterStateMismatch(expected, actual) == 2);
}

} // namespace

int main()
{
    TestPixelShaderStateAndPatchRecords();
    TestVertexNullShader();
    TestPreviousShaderFenceStamp();
    TestDeferredReleaseUsesRetainedBody();
    TestMalformedPatchUsesRetainedBody();
    TestPatchCopyPreservesGuestForwardOverlap();
    TestAuditComparatorNegativeControl();
}
