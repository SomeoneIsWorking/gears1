#pragma once

#include <cstddef>

#include <image.h>

#include "guest_memory.h"

namespace gears
{

// Points each variable-import thunk slot at real guest storage. XenonUtils
// leaves these holding an unresolved ordinal record, which faults as soon as
// the game dereferences one.
size_t ResolveImportVariables(GuestMemory& memory, const Image& image);

} // namespace gears
