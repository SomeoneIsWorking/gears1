#pragma once

#include <cstdint>

// Controller input, from the host to the guest's XamInput* imports.
//
// The console's XInput state is a small fixed structure; everything here exists
// to fill it honestly. Two host sources feed it:
//
//  - a real gamepad or the keyboard, polled by the presenter thread (which owns
//    SDL and its event queue);
//  - GEARS_INPUT_SCRIPT, a timed list of button states, which is how a headless
//    run drives the title reproducibly.
//
// When neither source is available the pad reports DISCONNECTED rather than
// connected-and-idle. That distinction matters: a title handles an absent
// controller, but a connected pad whose buttons never change reads as a player
// who simply is not pressing anything, and can leave the title waiting at a
// "press start" prompt for ever.
namespace gears
{

// X_INPUT_GAMEPAD_* -- the console's own button bits.
constexpr uint16_t kPadDpadUp = 0x0001;
constexpr uint16_t kPadDpadDown = 0x0002;
constexpr uint16_t kPadDpadLeft = 0x0004;
constexpr uint16_t kPadDpadRight = 0x0008;
constexpr uint16_t kPadStart = 0x0010;
constexpr uint16_t kPadBack = 0x0020;
constexpr uint16_t kPadLeftThumb = 0x0040;
constexpr uint16_t kPadRightThumb = 0x0080;
constexpr uint16_t kPadLeftShoulder = 0x0100;
constexpr uint16_t kPadRightShoulder = 0x0200;
constexpr uint16_t kPadGuide = 0x0400;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadX = 0x4000;
constexpr uint16_t kPadY = 0x8000;

// The console's X_INPUT_GAMEPAD, in host byte order. The XamInput
// implementation is what swaps it on the way into guest memory.
struct PadState
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;

    bool operator==(const PadState&) const = default;
};

// Opens the script source if GEARS_INPUT_SCRIPT is set, and reports whether any
// input source exists at all. Called once at start-up, before the presenter.
void InitialiseInput(bool haveWindow);

// True when a pad should be reported to the guest as connected.
bool PadConnected();

// The current state, and the packet number that increments whenever it changes
// (the console's contract: an unchanged packet number means nothing happened).
PadState CurrentPad(uint32_t& packetNumber);

// Called from the presenter thread on every event pump: reads SDL's gamepad and
// keyboard. A no-op in a headless run.
void PollHostInput();

// Advances the scripted source to `elapsedMs` since start. Called from
// whichever thread polls; safe to call from either.
void UpdateScriptedInput();

} // namespace gears
