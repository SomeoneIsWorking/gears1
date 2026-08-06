// See scripted_input.h for why a headless run needs a controller at all.
#include "scripted_input.h"

#include <algorithm>
#include <cstdlib>
#include <map>

#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/xbox.h"

#include <cstring>

namespace gears {

// The X_* status codes and input structs are Xenia's, and this file implements
// one of its interfaces; naming them unqualified keeps the overrides readable
// and identical in shape to Xenia's own drivers.
using namespace xe;
using namespace xe::hid;
using namespace std::chrono_literals;

namespace {

// Only the buttons a menu walk needs. Deliberately not the whole pad: an
// unknown name in a schedule is REFUSED (see ParseInputScript) rather than
// mapped to zero, because a press that silently becomes "no button" looks
// exactly like a game that ignored it.
const std::map<std::string, uint16_t>& ButtonNames() {
  static const std::map<std::string, uint16_t> names = {
      {"A", 0x1000},     {"B", 0x2000},         {"X", 0x4000},
      {"Y", 0x8000},     {"START", 0x0010},     {"BACK", 0x0020},
      {"UP", 0x0001},    {"DOWN", 0x0002},      {"LEFT", 0x0004},
      {"RIGHT", 0x0008},
  };
  return names;
}

}  // namespace

bool ParseInputScript(const std::string& text, std::vector<ScriptedPress>& out,
                      std::string& error_out) {
  out.clear();
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t comma = text.find(',', pos);
    std::string token =
        text.substr(pos, comma == std::string::npos ? std::string::npos
                                                    : comma - pos);
    pos = comma == std::string::npos ? text.size() + 1 : comma + 1;
    if (token.empty()) {
      continue;
    }
    // BUTTON@SECONDS[+REPEAT_SECONDS]
    size_t at = token.find('@');
    if (at == std::string::npos) {
      error_out = "'" + token + "' has no '@': expected BUTTON@SECONDS";
      return false;
    }
    std::string name = token.substr(0, at);
    std::transform(name.begin(), name.end(), name.begin(), ::toupper);

    // A STICK token: L/R, then X/Y, then +, - or 0. Parsed before the button
    // table so that an axis name never falls through to "not a button", which
    // would refuse a perfectly good schedule.
    bool is_stick = name.size() == 3 && (name[0] == 'L' || name[0] == 'R') &&
                    (name[1] == 'X' || name[1] == 'Y') &&
                    (name[2] == '+' || name[2] == '-' || name[2] == '0');
    if (is_stick) {
      std::string timing_s = token.substr(at + 1);
      size_t plus_s = timing_s.find('+');
      if (plus_s != std::string::npos) {
        timing_s = timing_s.substr(0, plus_s);
      }
      ScriptedPress stick;
      stick.axis = name[1] == 'X' ? 'x' : 'y';
      stick.right = name[0] == 'R';
      // Full deflection. 32767 rather than a partial value because the walk
      // this mirrors uses a full push, and a smaller one would move the
      // player a different distance in the same time -- which is exactly the
      // divergence this feature exists to remove.
      stick.value = name[2] == '+' ? int16_t(32767)
                                   : (name[2] == '-' ? int16_t(-32768) : int16_t(0));
      // Raw units, as for buttons above.
      stick.at =
          std::chrono::milliseconds(int64_t(std::atof(timing_s.c_str())));
      out.push_back(stick);
      continue;
    }

    auto it = ButtonNames().find(name);
    if (it == ButtonNames().end()) {
      error_out = "'" + name + "' is neither a button nor a stick this driver"
                  " knows (sticks are LX/LY/RX/RY with +, - or 0)";
      return false;
    }
    std::string timing = token.substr(at + 1);
    double repeat_seconds = 0.0;
    size_t plus = timing.find('+');
    if (plus != std::string::npos) {
      repeat_seconds = std::atof(timing.substr(plus + 1).c_str());
      timing = timing.substr(0, plus);
    }
    double at_seconds = std::atof(timing.c_str());
    ScriptedPress press;
    press.button = it->second;
    // RAW UNITS, scaled at use time. The schedule's numbers mean SECONDS on a
    // wall-clock run and GUEST FRAMES under --oracle_by_frame, and this parser
    // cannot know which. Baking in a *1000 here made every frame-driven
    // schedule fire at frame 150000 instead of frame 150 -- i.e. never. That
    // is not a hypothetical: measured, a 1200-frame run with
    // "START@150+270,A@300+120" reported ZERO button presses while capturing
    // all 3 of its frames, so tools/oracle_lockstep.sh has been comparing our
    // walked runtime against an oracle sitting on the title screen.
    press.at = std::chrono::milliseconds(int64_t(at_seconds));
    press.repeat = std::chrono::milliseconds(int64_t(repeat_seconds));
    out.push_back(press);
  }
  if (out.empty()) {
    error_out = "the schedule is empty, so nothing would ever be pressed";
    return false;
  }
  return true;
}

ScriptedInputDriver::ScriptedInputDriver(xe::ui::Window* window,
                                         size_t window_z_order,
                                         std::vector<ScriptedPress> presses)
    : InputDriver(window, window_z_order),
      presses_(std::move(presses)),
      start_(std::chrono::steady_clock::now()) {}

ScriptedInputDriver::~ScriptedInputDriver() = default;

xe::X_STATUS ScriptedInputDriver::Setup() { return X_STATUS_SUCCESS; }

// One schedule unit expressed in the units NowTick() returns: a guest frame
// when a frame tick source is installed, otherwise a millisecond.
uint64_t ScriptedInputDriver::UnitScale() const { return tick_source_ ? 1 : 1000; }

// How long a press is held. 120 ms on a wall clock; under frame indexing that
// same 120 would be 120 FRAMES (four seconds), so hold a handful of frames
// instead -- long enough for a title polling once a frame to see it.
uint64_t ScriptedInputDriver::HoldTicks() const { return tick_source_ ? 8 : 120; }

uint16_t ScriptedInputDriver::ButtonsAt(
    uint64_t tick) const {
  uint16_t buttons = 0;
  for (const ScriptedPress& press : presses_) {
    if (press.axis != 0) {
      continue;  // a stick deflection, handled by SticksAt
    }
    const uint64_t at = static_cast<uint64_t>(press.at.count()) * UnitScale();
    const uint64_t hold = HoldTicks();
    const uint64_t repeat =
        static_cast<uint64_t>(press.repeat.count()) * UnitScale();
    if (tick < at) {
      continue;
    }
    uint64_t since = tick - at;
    if (repeat > 0) {
      since = since % repeat;
    }
    if (since < hold) {
      buttons |= press.button;
    }
  }
  return buttons;
}

// The current tick: the guest's frame counter when one has been supplied,
// otherwise milliseconds since the driver was made. Both are monotonic, which
// is all ButtonsAt needs.
uint64_t ScriptedInputDriver::NowTick() const {
  if (tick_source_) {
    return tick_source_();
  }
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start_)
          .count());
}

xe::X_RESULT ScriptedInputDriver::GetCapabilities(
    uint32_t user_index, uint32_t flags,
    xe::hid::X_INPUT_CAPABILITIES* out_caps) {
  // Only player one exists. Reporting a pad on every index would make the title
  // believe in four players, which is not the thing being emulated here.
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;      // XINPUT_DEVCTYPE_GAMEPAD
  out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  out_caps->flags = 0;
  return X_ERROR_SUCCESS;
}

xe::X_RESULT ScriptedInputDriver::GetState(uint32_t user_index,
                                           xe::hid::X_INPUT_STATE* out_state) {
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const uint64_t tick = NowTick();
  const uint16_t buttons = ButtonsAt(tick);
  if (buttons != last_buttons_) {
    // The packet number is how a title notices anything happened; leaving it
    // constant makes every press invisible however correct the button mask is.
    ++packet_number_;
    if (buttons && !last_buttons_) {
      ++presses_reported_;
      XELOGI("oracle input: button mask {:04X} at {} {} (press {})", buttons,
             tick, tick_source_ ? "guest frames" : "ms", presses_reported_);
    }
    last_buttons_ = buttons;
  }
  // Each axis takes the LAST token at or before now, so a deflection is held
  // until something changes it -- the same semantics as the runtime's script.
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint64_t best[4] = {0, 0, 0, 0};
  bool seen[4] = {false, false, false, false};
  for (const ScriptedPress& press : presses_) {
    if (press.axis == 0) {
      continue;
    }
    const uint64_t at = static_cast<uint64_t>(press.at.count()) * UnitScale();
    if (tick < at) {
      continue;
    }
    const int idx = (press.right ? 2 : 0) + (press.axis == 'y' ? 1 : 0);
    if (seen[idx] && at < best[idx]) {
      continue;
    }
    seen[idx] = true;
    best[idx] = at;
    (idx == 0 ? lx : idx == 1 ? ly : idx == 2 ? rx : ry) = press.value;
  }
  if (lx != last_lx_ || ly != last_ly_ || rx != last_rx_ || ry != last_ry_) {
    ++packet_number_;
    XELOGI("oracle input: sticks L({},{}) R({},{}) at {} {}", lx, ly, rx, ry,
           tick, tick_source_ ? "guest frames" : "ms");
    last_lx_ = lx; last_ly_ = ly; last_rx_ = rx; last_ry_ = ry;
  }

  std::memset(out_state, 0, sizeof(*out_state));
  out_state->packet_number = packet_number_;
  out_state->gamepad.buttons = buttons;
  out_state->gamepad.thumb_lx = lx;
  out_state->gamepad.thumb_ly = ly;
  out_state->gamepad.thumb_rx = rx;
  out_state->gamepad.thumb_ry = ry;
  return X_ERROR_SUCCESS;
}

xe::X_RESULT ScriptedInputDriver::SetState(
    uint32_t user_index, xe::hid::X_INPUT_VIBRATION* vibration) {
  // Rumble is accepted and discarded: refusing it would tell the title the pad
  // vanished.
  return user_index == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

xe::X_RESULT ScriptedInputDriver::GetKeystroke(
    uint32_t user_index, uint32_t flags,
    xe::hid::X_INPUT_KEYSTROKE* out_keystroke) {
  // No keystrokes: this is a gamepad, and a title that reads keystrokes gets
  // the same answer a real pad gives.
  return X_ERROR_EMPTY;
}

xe::hid::InputType ScriptedInputDriver::GetInputType() const {
  return xe::hid::InputType::Controller;
}

}  // namespace gears
