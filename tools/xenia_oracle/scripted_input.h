// A controller that presses buttons on a schedule, for a run with no operator.
//
// Xenia's nop HID driver reports X_ERROR_DEVICE_NOT_CONNECTED for everything,
// and carries the TODO that explains why that is not enough: "spoof a device so
// that games don't stop waiting for a controller to be plugged in". This title
// does exactly that -- and beyond the wait, it needs actual presses to leave the
// title screen and the storage-device dialog, so a headless run that only sits
// there renders black frames for as long as you care to watch.
//
// The schedule is deliberately dumb: press, hold briefly, release, repeat. That
// is what a person does to get into this game, and it needs no model of which
// menu is on screen.
#ifndef GEARS_XENIA_ORACLE_SCRIPTED_INPUT_H_
#define GEARS_XENIA_ORACLE_SCRIPTED_INPUT_H_

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "xenia/hid/input_driver.h"

namespace gears {

// One scheduled press: `button` held for `hold` starting at `at`, and repeated
// every `repeat` if that is non-zero.
struct ScriptedPress {
  uint16_t button = 0;
  std::chrono::milliseconds at{0};
  std::chrono::milliseconds hold{120};
  std::chrono::milliseconds repeat{0};
};

// Parses "START@25,A@30+1" -- button at second, optionally repeating every N
// seconds. Returns false and says which token failed rather than silently
// dropping it: a schedule that quietly lost its only press produces a run that
// looks like the game ignored input.
bool ParseInputScript(const std::string& text, std::vector<ScriptedPress>& out,
                      std::string& error_out);

class ScriptedInputDriver final : public xe::hid::InputDriver {
 public:
  ScriptedInputDriver(xe::ui::Window* window, size_t window_z_order,
                      std::vector<ScriptedPress> presses);
  ~ScriptedInputDriver() override;

  xe::X_STATUS Setup() override;

  xe::X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                               xe::hid::X_INPUT_CAPABILITIES* out_caps) override;
  xe::X_RESULT GetState(uint32_t user_index,
                        xe::hid::X_INPUT_STATE* out_state) override;
  xe::X_RESULT SetState(uint32_t user_index,
                        xe::hid::X_INPUT_VIBRATION* vibration) override;
  xe::X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                            xe::hid::X_INPUT_KEYSTROKE* out_keystroke) override;
  xe::hid::InputType GetInputType() const override;

  // How many distinct presses have been reported so far. A run that ends with
  // zero has not been driven at all, however healthy its log looks.
  uint32_t presses_reported() const { return presses_reported_; }

  // DRIVE THE SCHEDULE BY GUEST FRAMES INSTEAD OF WALL CLOCK.
  //
  // Two emulators of the same title run at different speeds and load at
  // different rates, so a schedule keyed to the wall clock reaches a different
  // point in the game on each -- which is exactly why a pixel metric between
  // our filmstrip and the oracle's has always been meaningless. Keyed to the
  // guest's OWN frame counter, "frame 1500" is the same game moment on both
  // sides for as long as the title is deterministic under identical input.
  //
  // `source` returns the current guest frame; the schedule's numbers are then
  // read as frames rather than milliseconds. Unset, the driver uses the clock
  // exactly as before.
  void SetFrameTickSource(std::function<uint64_t()> source) {
    tick_source_ = std::move(source);
  }
  bool frame_driven() const { return static_cast<bool>(tick_source_); }

 private:
  uint16_t ButtonsAt(uint64_t tick) const;
  uint64_t NowTick() const;

  std::vector<ScriptedPress> presses_;
  std::chrono::steady_clock::time_point start_;
  std::function<uint64_t()> tick_source_;
  uint16_t last_buttons_ = 0;
  uint32_t packet_number_ = 1;
  uint32_t presses_reported_ = 0;
};

}  // namespace gears

#endif  // GEARS_XENIA_ORACLE_SCRIPTED_INPUT_H_
