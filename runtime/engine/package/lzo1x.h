#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace gears::engine::package
{

// A compressed block that does not decode to exactly its declared size.
class DecompressionError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

// Decodes one LZO1X block, the codec of Gears' cooked-package chunks.
// Independently written from the published LZO1X bitstream description. The
// decoder is bounded on both sides: it refuses a stream that reads past its
// input, writes past `output`, references data before the output start, ends
// without the end-of-stream marker, or leaves input or output unconsumed.
void DecodeLzo1x(std::span<const std::uint8_t> input, std::span<std::uint8_t> output);

} // namespace gears::engine::package
