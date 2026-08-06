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

// WHAT ADVANCES THE CLOCK. GEARS_GUEST_CLOCK_TRIGGER, one of:
//
//   present         (default) VdSwap. MEASURED TO DEADLOCK THIS TITLE: it spins
//                   in guest code waiting for time before its first present, so
//                   time cannot advance until a frame is presented and a frame
//                   cannot be presented until time advances. 0 frames in 100 s
//                   against 9 for the real clock. Kept as the default because it
//                   is the honest reading of "a step per frame", and because a
//                   mode that silently substituted another trigger would make
//                   the deadlock look fixed.
//   vblank          The 60 Hz vblank thread, which fires whether or not the
//                   guest presents, so it cannot deadlock -- but it is paced by
//                   a HOST SLEEP, so a run under it is NOT reproducible. This is
//                   the control arm that proved the deadlock is the trigger and
//                   not the virtual clock; never quote a comparison from it.
//   vblank-freerun  Vblank, delivered as soon as the guest has consumed the
//                   previous one rather than on a host sleep. MEASURED TO FREEZE
//                   THE PICTURE: the title keeps presenting (the counter reaches
//                   10,800) but from about frame 2,700 every presented frame is
//                   BIT-IDENTICAL to the last, where the real-clock control's
//                   consecutive frames are 21-34% identical, i.e. alive. Nothing
//                   is moving. NOT USABLE for a comparison.
//
// THE TRAP THIS MODE SETS, because it nearly landed. A frozen picture is
// trivially reproducible, so the determinism control it is meant to pass looks
// PERFECT under it -- frames 3,300 and 7,800 bit-identical across independent
// runs, against 17.65% at frame 1,200 on the real clock. That is a match on a
// still image, not on reproduced gameplay. Any determinism number from this
// mode must be read together with "does the picture change at all", and the
// real-clock arm is the only control that answers it.
//
// The likely mechanism, not yet confirmed: free-running vblanks arrive far
// faster than 60 Hz, so guest time advances at hundreds of times real rate and
// the title's per-frame delta becomes absurd.
enum class ClockTrigger { kPresent, kVblank, kVblankFreeRun };
ClockTrigger GuestClockTrigger();

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
