#pragma once

#include <cstdint>

namespace gears
{

// THE CLOCK THE GUEST READS, in one place, so it can be made a function of the
// input schedule instead of of the host's speed.
//
// WHY THIS EXISTS. Catalog #84: our own renderer, same binary, same input script
// indexed by the guest's own frame counter, two runs -- 98.96% identical pixels
// at frame 300, 25.90% at 600, 17.65% at 1200. The title does not reproduce
// against ITSELF, so no indexing scheme can make "frame N" the same game moment
// on two emulators, and every cross-side pixel number past the first few hundred
// frames measures our nondeterminism more than it measures the oracle.
//
// The reason is that UE3 advances animation and physics on DELTA TIME, not on a
// frame count. Emulator speed varies, so the same frame index is a different
// amount of GUEST TIME and the simulation really is at a different point.
//
// So: when a fixed step is configured, every clock the guest can read advances
// by exactly that much per PRESENTED FRAME and by nothing else. The simulation
// becomes a function of the input schedule alone, and two runs -- on one
// emulator or on two -- reach the same state at the same frame.
//
// THIS IS A HARNESS MODE, NOT THE DEFAULT. A fixed step decouples the guest's
// clock from real time, so audio, spin-waits bounded in timebase ticks, and
// anything that waits on a deadline see a clock that only moves when a frame is
// presented. That is exactly what a comparison wants and is NOT what a person
// playing the game wants. Off unless GEARS_GUEST_CLOCK_STEP_NS is set.

// Nanoseconds the guest's clocks have advanced. Real elapsed host time unless a
// fixed step is configured, in which case frames * step.
uint64_t GuestClockNanoseconds();

// Called once per presented frame (VdSwap). A no-op in real-time mode -- the
// call site does not need to know which mode is active.
void AdvanceGuestClockFrame();

// CONTROL ARM, not a mode to ship a comparison on. GEARS_GUEST_CLOCK_ON_VBLANK=1
// advances the clock from the 60 Hz VBLANK thread instead of from VdSwap. Vblank
// is paced by a host sleep and fires whether or not the guest presents, so it
// cannot deadlock -- which makes it the discriminator between "the title stalls
// because the clock is frozen before the first frame" and "the title stalls for
// some other reason and the clock is a red herring". It is HOST-PACED and
// therefore NOT reproducible; never quote a comparison taken under it.
bool GuestClockOnVblank();

// Whether a fixed step is in effect, and how big it is. For reporting: a run
// that silently used the real clock and one that used a fixed step produce
// different data and must not be told apart by guesswork.
bool GuestClockIsFixedStep();
uint64_t GuestClockStepNanoseconds();

// Reads GEARS_GUEST_CLOCK_STEP_NS and reports what it decided, ALWAYS -- both
// when a step is configured and when one is not. "the clock is fixed" and "I
// never looked at the variable" must not print the same way. Idempotent.
void InitGuestClock();

} // namespace gears
