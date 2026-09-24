#include "lzo1x.h"

#include <algorithm>
#include <optional>
#include <string>

namespace gears::engine::package
{
namespace
{

// Cursor pair over one block. Every read and write goes through the checked
// operations below, so a corrupt block fails at the first bad reference.
class Lzo1xDecoder
{
  public:
    Lzo1xDecoder(std::span<const std::uint8_t> input, std::span<std::uint8_t> output)
        : input_(input), output_(output)
    {
    }

    void Decode()
    {
        std::size_t state = DecodeLeadingLiterals();
        while (true)
        {
            std::uint8_t instruction = ReadByte();
            if (instruction >= 64U)
            {
                state = ShortMatch(instruction);
            }
            else if (instruction >= 32U)
            {
                state = MediumMatch(instruction);
            }
            else if (instruction >= 16U)
            {
                std::optional<std::size_t> next = LongMatch(instruction);
                if (!next)
                {
                    break;
                }
                state = *next;
            }
            else
            {
                state = LowInstruction(instruction, state);
            }
        }
        if (in_ != input_.size() || out_ != output_.size())
        {
            Fail("block ended with " + std::to_string(input_.size() - in_) +
                 " unread input byte(s) and " + std::to_string(output_.size() - out_) +
                 " unwritten output byte(s)");
        }
    }

  private:
    [[noreturn]] static void Fail(const std::string &reason)
    {
        throw DecompressionError("LZO1X: " + reason);
    }

    std::uint8_t ReadByte()
    {
        if (in_ >= input_.size())
        {
            Fail("input ends inside an instruction");
        }
        return input_[in_++];
    }

    std::size_t ReadLittleEndian16()
    {
        std::size_t low = ReadByte();
        std::size_t high = ReadByte();
        return low | (high << 8U);
    }

    // A zero length field is followed by zero bytes worth 255 each and one
    // final non-zero byte; `base` is the field's all-ones value.
    std::size_t ReadExtendedLength(std::size_t base)
    {
        std::size_t length = 0;
        std::uint8_t next = ReadByte();
        while (next == 0U)
        {
            length += 255U;
            if (length > output_.size())
            {
                Fail("extended length exceeds the block");
            }
            next = ReadByte();
        }
        return length + base + next;
    }

    void CopyLiterals(std::size_t count)
    {
        if (count > input_.size() - in_)
        {
            Fail("literal run reads past the input");
        }
        if (count > output_.size() - out_)
        {
            Fail("literal run writes past the output");
        }
        std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(in_), count,
                    output_.begin() + static_cast<std::ptrdiff_t>(out_));
        in_ += count;
        out_ += count;
    }

    // Byte-by-byte, because an LZO match may overlap its own output.
    void CopyMatch(std::size_t distance, std::size_t length)
    {
        if (distance == 0U || distance > out_)
        {
            Fail("match distance " + std::to_string(distance) + " precedes the output start");
        }
        if (length > output_.size() - out_)
        {
            Fail("match writes past the output");
        }
        for (std::size_t i = 0; i < length; ++i)
        {
            output_[out_] = output_[out_ - distance];
            ++out_;
        }
    }

    // Trailing literal count carried in the low two bits of a match.
    std::size_t FinishMatch(std::size_t trailing)
    {
        CopyLiterals(trailing);
        return trailing;
    }

    std::size_t DecodeLeadingLiterals()
    {
        if (input_.empty())
        {
            Fail("empty block");
        }
        if (input_[0] <= 17U)
        {
            return 0;
        }
        std::size_t count = ReadByte() - 17U;
        CopyLiterals(count);
        return std::min<std::size_t>(count, 4U);
    }

    // 0 0 0 0 L L L L: a literal run after a match without trailing literals;
    // otherwise a two-byte (after 1..3 literals) or three-byte (after a run)
    // near match.
    std::size_t LowInstruction(std::uint8_t instruction, std::size_t state)
    {
        if (state == 0U)
        {
            std::size_t field = instruction & 15U;
            CopyLiterals(3U + (field == 0U ? ReadExtendedLength(15U) : field));
            return 4U;
        }
        std::size_t high = ReadByte();
        std::size_t low = (instruction >> 2U) & 3U;
        if (state == 4U)
        {
            CopyMatch((high << 2U) + low + 2049U, 3U);
        }
        else
        {
            CopyMatch((high << 2U) + low + 1U, 2U);
        }
        return FinishMatch(instruction & 3U);
    }

    // 0 1 L D D D S S and 1 L L D D D S S: matches of 3..8 bytes within 2 KiB.
    std::size_t ShortMatch(std::uint8_t instruction)
    {
        std::size_t length =
            instruction >= 128U ? 5U + ((instruction >> 5U) & 3U) : 3U + ((instruction >> 5U) & 1U);
        std::size_t high = ReadByte();
        CopyMatch((high << 3U) + ((instruction >> 2U) & 7U) + 1U, length);
        return FinishMatch(instruction & 3U);
    }

    // 0 0 1 L L L L L: a match within 16 KiB.
    std::size_t MediumMatch(std::uint8_t instruction)
    {
        std::size_t field = instruction & 31U;
        std::size_t length = 2U + (field == 0U ? ReadExtendedLength(31U) : field);
        std::size_t word = ReadLittleEndian16();
        CopyMatch((word >> 2U) + 1U, length);
        return FinishMatch(word & 3U);
    }

    // 0 0 0 1 H L L L: a far match, or the end-of-stream marker when its
    // distance field is zero. Returns no state at the end of the stream.
    std::optional<std::size_t> LongMatch(std::uint8_t instruction)
    {
        std::size_t field = instruction & 7U;
        std::size_t length = 2U + (field == 0U ? ReadExtendedLength(7U) : field);
        std::size_t word = ReadLittleEndian16();
        std::size_t distance = 16384U + ((instruction & 8U) << 11U) + (word >> 2U);
        if (distance == 16384U)
        {
            if (length != 3U)
            {
                Fail("malformed end-of-stream marker");
            }
            return std::nullopt;
        }
        CopyMatch(distance, length);
        return FinishMatch(word & 3U);
    }

    std::span<const std::uint8_t> input_;
    std::span<std::uint8_t> output_;
    std::size_t in_ = 0;
    std::size_t out_ = 0;
};

} // namespace

void DecodeLzo1x(std::span<const std::uint8_t> input, std::span<std::uint8_t> output)
{
    Lzo1xDecoder(input, output).Decode();
}

} // namespace gears::engine::package
