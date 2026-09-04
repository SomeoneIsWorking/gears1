#pragma once

#include <cstdint>

#include "guest_memory.h"

namespace gears
{

// The XMA hardware block's register window and its context array.
//
// XMA is the console's audio decoder. The title does NOT reach it through
// kernel imports -- the whole image imports only XMACreateContext and
// XMAReleaseContext -- so everything else is its own statically linked library
// poking the hardware registers at 0x7FEA0000 directly. That means the register
// block is not optional decoration around a decoder: it is the interface, and
// getting it wrong makes the decoder unreachable no matter how good it is.
//
// The one contract that matters before any decoding exists: register 0x600
// publishes the address of the CONTEXT ARRAY, and the title reads it (with
// lwbrx, at sub_825E8FE0) to learn where contexts live. Every context is then
// identified by its INDEX in that array, and the kick/lock/clear registers are
// bitmaps over those indices. Leaving the register zero, as the runtime did,
// left the title computing indices against a base of nothing.
bool SetupXmaRegisters(GuestMemory &memory);

// A context is a 64-byte slot in the array, not an arbitrary allocation. The
// title's index arithmetic only works if contexts come from this array.
uint32_t AllocateXmaContext();
void ReleaseXmaContext(uint32_t guestPointer);

// A store to the XMA register window, delivered from the device-store hook
// through x360port/Xenia's device-memory callback as it happens rather than
// sampled afterwards. Returns false if
// the address is not one of ours, so the caller can go on looking.
bool OnXmaRegisterStore(uint32_t address, uint32_t value);

} // namespace gears
