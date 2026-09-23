#pragma once

#include <string>
#include <string_view>

#include "input.h"

namespace gears::product
{

// A pad state from URL-encoded fields: buttons=A,START (the names scripted
// input uses), lx/ly/rx/ry in -32767..32767 and lt/rt in 0..255. Absent fields
// are neutral. Returns false with a reason naming the field on any unknown
// field, unknown button, or out-of-range value.
[[nodiscard]] bool ParsePadForm(std::string_view encoded, PadState &pad, std::string &error);

} // namespace gears::product
