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

// Lays out the loader's record of the running executable in guest memory: a
// module entry whose +0x58 field points at a copy of the real XEX header
// region (the layout titles depend on -- they read the header pointer from
// the module entry and hand it to RtlImageXexHeaderField). Must run before
// ResolveImportVariables so the XexExecutableModuleHandle variable can carry
// the installed handle.
bool InstallExecutableModule(GuestMemory& memory, const uint8_t* xexData, size_t xexSize);

// The handle the kernel uses to name the running executable: the guest
// address of the module entry installed above. Every path that can produce
// the handle agrees, since the title both reads the XexExecutableModuleHandle
// variable and asks XexGetModuleHandle for the same thing and may compare the
// two.
uint32_t ExecutableModuleHandle();

// The parsed image stays reachable after boot: imports that answer questions
// about the running executable (its sections, its handle) need it, and the
// alternative is duplicating the loader's parse per import.
void SetLoadedImage(const Image& image);
const Image* LoadedImage();

} // namespace gears
