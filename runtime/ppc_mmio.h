#pragma once

#include <cstdint>

// The device-store hook.
//
// The recompiler emits PPC_MM_STORE_* rather than a plain store wherever it
// decided a store is memory-mapped I/O, and leaves those macros #ifndef-guarded
// so a runtime can take them over. This header is force-included ahead of
// ppc_context.h (see runtime/CMakeLists.txt) to do exactly that.
//
// It is worth taking over because a device write is an EVENT, not a value. The
// XMA block's kick register is the case that forced this: the title writes a
// bitmap of contexts to decode and then overwrites it with the next bitmap, so
// anything that samples the register instead of seeing the write can only
// report which contexts it happened to catch -- two kicks between samples are
// indistinguishable from one. A decoder driven that way drops work silently.
//
// The cost is nothing: the whole recompiled image contains ELEVEN
// PPC_MM_STORE_U32 sites and no PPC_MM_LOAD sites at all, so this is not on any
// hot path. Reads need no hook because the registers live in real guest memory
// and an ordinary load already sees them.
namespace gears
{
void DeviceStore32(uint32_t address, uint32_t value);
}

#define PPC_MM_STORE_U32(x, y) gears::DeviceStore32((x), (y))
