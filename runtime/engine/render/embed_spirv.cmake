# Writes a SPIR-V binary as a C++ header holding one inline constexpr array.
# Inputs: SPIRV (binary path), HEADER (output path), SYMBOL (array name).
file(READ "${SPIRV}" bytes HEX)
string(LENGTH "${bytes}" hex_length)
math(EXPR word_count "${hex_length} / 8")
set(words "")
set(position 0)
while(position LESS hex_length)
    string(SUBSTRING "${bytes}" ${position} 8 word)
    # SPIR-V is little-endian: reverse the four bytes of each word.
    string(SUBSTRING "${word}" 0 2 b0)
    string(SUBSTRING "${word}" 2 2 b1)
    string(SUBSTRING "${word}" 4 2 b2)
    string(SUBSTRING "${word}" 6 2 b3)
    string(APPEND words "0x${b3}${b2}${b1}${b0}U,")
    math(EXPR position "${position} + 8")
endwhile()
file(WRITE "${HEADER}"
    "#pragma once\n#include <array>\n#include <cstdint>\n"
    "namespace gears::engine::render::spirv {\n"
    "inline constexpr std::array<std::uint32_t, ${word_count}> ${SYMBOL}{${words}};\n"
    "}\n")
