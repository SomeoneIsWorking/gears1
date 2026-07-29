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

// Which of the console's six hardware threads this guest thread runs on.
//
// The runtime does not place host threads by it -- the host scheduler is
// better at that than a six-way pin -- but the NUMBER is guest-visible state
// the console maintains, and title code reads it: the audio worker indexes a
// per-CPU rendezvous array with it (catalog #40). It lives in two places, the
// KPCR's embedded KPRCB and the KTHREAD, and both are written together.
//
// `threadObject` may be 0 for the host threads the runtime drives into guest
// code, which have a KPCR but no KTHREAD of their own.
void SetGuestThreadProcessor(GuestMemory& memory, uint32_t pcrAddress,
                             uint32_t threadObject, uint8_t cpu);

// The console has six hardware threads, so a legal processor number is 0..5.
// Callers check against this to tell a processor from the sentinels below.
constexpr uint8_t kHardwareThreadCount = 6;

// Answers ProcessorNumberFromMask cannot express as a processor. Both are out
// of the console's 0..5 range so neither can be mistaken for one, and they are
// distinct because the callers must do different things with them.
//
// kProcessorInherit: the mask is EMPTY, which the console reads as "wherever my
// creator runs" -- only the caller knows who that is.
constexpr uint8_t kProcessorInherit = 0xFF;
// kProcessorNone: the mask names no hardware thread this console has. Nothing
// can be inferred from it, so the thread must be left where it was.
constexpr uint8_t kProcessorNone = 0xFE;

// The console's mapping from an affinity/processor MASK to a processor number:
// the index of the set bit IS the processor. Both places the title names a
// processor -- the top byte of ExCreateThread's creation flags and
// KeSetAffinityThread's affinity argument -- come through here, so the two
// cannot drift apart.
//
// Ground truth is Xenia's GetFakeCpuNumber (xenia/kernel/xthread.cc:157), which
// asserts a well-formed mask names exactly one of six processors and nothing
// above bit 5.
uint8_t ProcessorNumberFromMask(uint32_t mask);

// The processor number of the thread calling this, for the parent-inheritance
// rule above.
uint8_t CurrentGuestProcessor();
void SetCurrentGuestProcessor(uint8_t cpu);

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
