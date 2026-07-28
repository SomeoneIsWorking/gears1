#pragma once

#include <cstdint>
#include <string>

#include "guest_memory.h"

namespace gears
{

// On the Xbox 360 r13 holds the address of the current thread's KPCR
// (Processor Control Region); guest code reaches the thread block, TLS and the
// stack bounds through it. The kernel sets this up before a title runs, so the
// runtime has to do the same or the first r13-relative load faults.
struct GuestThreadBlock
{
    uint32_t pcrAddress;
    uint32_t threadAddress;
    uint32_t stackBase; // highest address, stack grows down
    uint32_t stackLimit;
};

// Allocates and populates a thread block, returning the value for r13.
bool CreateGuestThreadBlock(GuestMemory& memory, uint32_t stackSize, GuestThreadBlock& out);

// Which guest thread is running on this host thread, for diagnostics.
//
// A kernel log line that does not name its caller cannot distinguish the
// title's own worker threads from the host threads the runtime drives INTO
// guest code (the audio pump, the graphics ISR). That distinction is the whole
// question when a wait never returns: a title worker blocking is the title
// waiting for us, and a pump thread blocking is us stuck inside the title.
void SetGuestThreadName(std::string name);
const char* GuestThreadName();

} // namespace gears
