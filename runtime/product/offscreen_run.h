#pragma once

#include <x360port/system_session.hpp>

#include "product_options.h"

namespace gears::product
{

// Drives a launched offscreen session for the requested duration. Each second
// it logs presents, translated guest functions, and native-override calls, and
// captures guest output at the requested interval. The run succeeds only when
// the title presented, the dynarec translated guest code, and no function
// failed to translate. Native-override calls are reported, not required: an
// override bound to a path the scripted route never takes is correctly
// unreached.
[[nodiscard]] bool RunOffscreen(x360port::SystemSession &session, const ProductOptions &options);

} // namespace gears::product
