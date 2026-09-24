#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace gears::engine::package
{

// Content that violates the cooked-package format. Carries the byte offset of
// the first violation so a report names where the file stopped making sense.
class PackageFormatError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

enum class ByteOrder : std::uint8_t
{
    Big,
    Little,
};

// Bounded cursor over package bytes. Every read checks the remaining length
// and refuses rather than returning a default value.
class ByteReader
{
  public:
    ByteReader(std::span<const std::uint8_t> bytes, ByteOrder order) : bytes_(bytes), order_(order)
    {
    }

    [[nodiscard]] std::size_t Offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t Size() const noexcept { return bytes_.size(); }
    [[nodiscard]] std::size_t Remaining() const noexcept { return bytes_.size() - offset_; }

    void Seek(std::size_t offset);
    std::uint8_t ReadU8();
    std::uint32_t ReadU32();
    std::int32_t ReadI32();
    std::uint64_t ReadU64();
    std::span<const std::uint8_t> ReadBytes(std::size_t count);
    // A length-prefixed string: a positive length counts 8-bit characters, a
    // negative one counts UTF-16 code units; both include the terminator.
    // UTF-16 text is returned as UTF-8.
    std::string ReadString();
    // A count that sizes a following array; refuses a negative count or one
    // whose elements cannot fit in the remaining bytes.
    std::size_t ReadCount(std::size_t min_element_size);

    [[noreturn]] void Fail(const std::string &reason) const;

  private:
    std::span<const std::uint8_t> Take(std::size_t count);

    std::span<const std::uint8_t> bytes_;
    ByteOrder order_;
    std::size_t offset_ = 0;
};

} // namespace gears::engine::package
