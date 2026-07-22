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

// The handle the kernel uses to name the running executable. On hardware this
// is the address of the module's own structure, so the image base is the
// natural value; what matters here is that every path that can produce it
// agrees, since the title both reads the XexExecutableModuleHandle variable
// and asks XexGetModuleHandle for the same thing and may compare the two.
//
// Nothing lays out a module structure at this address yet, so a title that
// dereferences the handle rather than passing it back will fault on the field
// it reads. That is deliberate: it points straight at the missing work.
uint32_t ExecutableModuleHandle();

} // namespace gears
