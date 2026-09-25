#include "byte_reader.h"

#include <bit>
#include <format>
#include <limits>

namespace gears::engine::package
{
namespace
{

void AppendUtf8(std::string &out, std::uint32_t code_point)
{
    if (code_point < 0x80U)
    {
        out.push_back(static_cast<char>(code_point));
    }
    else if (code_point < 0x800U)
    {
        out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
    else
    {
        out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

} // namespace

void ByteReader::Fail(const std::string &reason) const
{
    throw PackageFormatError(std::format("at offset {:#x}: {}", offset_, reason));
}

std::span<const std::uint8_t> ByteReader::Take(std::size_t count)
{
    if (count > Remaining())
    {
        Fail("needs " + std::to_string(count) + " byte(s) but only " + std::to_string(Remaining()) +
             " remain");
    }
    std::span<const std::uint8_t> taken = bytes_.subspan(offset_, count);
    offset_ += count;
    return taken;
}

void ByteReader::Seek(std::size_t offset)
{
    if (offset > bytes_.size())
    {
        Fail(std::format("seek to {:#x} beyond {} byte(s)", offset, bytes_.size()));
    }
    offset_ = offset;
}

std::uint8_t ByteReader::ReadU8()
{
    return Take(1)[0];
}

std::uint16_t ByteReader::ReadU16()
{
    std::span<const std::uint8_t> b = Take(2);
    if (order_ == ByteOrder::Big)
    {
        return static_cast<std::uint16_t>((unsigned{b[0]} << 8U) | unsigned{b[1]});
    }
    return static_cast<std::uint16_t>((unsigned{b[1]} << 8U) | unsigned{b[0]});
}

std::uint32_t ByteReader::ReadU32()
{
    std::span<const std::uint8_t> b = Take(4);
    if (order_ == ByteOrder::Big)
    {
        return (std::uint32_t{b[0]} << 24U) | (std::uint32_t{b[1]} << 16U) |
               (std::uint32_t{b[2]} << 8U) | std::uint32_t{b[3]};
    }
    return (std::uint32_t{b[3]} << 24U) | (std::uint32_t{b[2]} << 16U) |
           (std::uint32_t{b[1]} << 8U) | std::uint32_t{b[0]};
}

std::int32_t ByteReader::ReadI32()
{
    return static_cast<std::int32_t>(ReadU32());
}

float ByteReader::ReadF32()
{
    return std::bit_cast<float>(ReadU32());
}

std::uint64_t ByteReader::ReadU64()
{
    std::uint64_t first = ReadU32();
    std::uint64_t second = ReadU32();
    return order_ == ByteOrder::Big ? (first << 32U) | second : (second << 32U) | first;
}

std::span<const std::uint8_t> ByteReader::ReadBytes(std::size_t count)
{
    return Take(count);
}

std::size_t ByteReader::ReadCount(std::size_t min_element_size)
{
    std::int32_t count = ReadI32();
    if (count < 0)
    {
        Fail("negative element count " + std::to_string(count));
    }
    auto elements = static_cast<std::size_t>(count);
    if (min_element_size != 0U && elements > Remaining() / min_element_size)
    {
        Fail("element count " + std::to_string(count) + " cannot fit in the remaining " +
             std::to_string(Remaining()) + " byte(s)");
    }
    return elements;
}

std::string ByteReader::ReadString()
{
    std::int32_t length = ReadI32();
    if (length == 0)
    {
        return {};
    }
    if (length == std::numeric_limits<std::int32_t>::min())
    {
        Fail("string length overflows");
    }
    std::string text;
    if (length > 0)
    {
        std::span<const std::uint8_t> chars = Take(static_cast<std::size_t>(length));
        if (chars.back() != 0U)
        {
            Fail("8-bit string is not terminated");
        }
        text.assign(chars.begin(), chars.end() - 1);
        return text;
    }
    auto units = static_cast<std::size_t>(-static_cast<std::int64_t>(length));
    if (units > Remaining() / 2U)
    {
        Fail("UTF-16 string of " + std::to_string(units) + " unit(s) exceeds the input");
    }
    for (std::size_t i = 0; i + 1 < units; ++i)
    {
        std::span<const std::uint8_t> unit = Take(2);
        std::uint32_t value = order_ == ByteOrder::Big ? (std::uint32_t{unit[0]} << 8U) | unit[1]
                                                       : (std::uint32_t{unit[1]} << 8U) | unit[0];
        AppendUtf8(text, value);
    }
    std::span<const std::uint8_t> terminator = Take(2);
    if (terminator[0] != 0U || terminator[1] != 0U)
    {
        Fail("UTF-16 string is not terminated");
    }
    return text;
}

} // namespace gears::engine::package
