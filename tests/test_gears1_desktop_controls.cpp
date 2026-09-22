#include "input.h"
#include "titles/gears1/desktop_controls.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace
{

using gears::titles::gears1::MapDesktopControls;
using x360port::DesktopKey;
using x360port::DesktopMouseButton;
using x360port::DesktopSnapshot;

void Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

DesktopSnapshot Keys(std::initializer_list<DesktopKey> keys)
{
    DesktopSnapshot desktop;
    for (DesktopKey key : keys)
    {
        desktop.keys.set(static_cast<std::size_t>(key));
    }
    return desktop;
}

void IdleDesktopIsANeutralPad()
{
    Check(MapDesktopControls(DesktopSnapshot{}) == gears::PadState{},
          "an idle keyboard and mouse moved the pad");
}

void MovementKeysDeflectTheLeftStick()
{
    gears::PadState forward = MapDesktopControls(Keys({DesktopKey::W, DesktopKey::D}));
    Check(forward.thumbLY == 32767 && forward.thumbLX == 32767,
          "W and D did not push the left stick fully up and right");
    gears::PadState back = MapDesktopControls(Keys({DesktopKey::S, DesktopKey::A}));
    Check(back.thumbLY == -32767 && back.thumbLX == -32767,
          "S and A did not push the left stick fully down and left");
    gears::PadState opposed = MapDesktopControls(Keys({DesktopKey::W, DesktopKey::S}));
    Check(opposed.thumbLY == 0, "opposing movement keys did not cancel");
}

void KeysAndButtonsMapToThePad()
{
    gears::PadState pad = MapDesktopControls(
        Keys({DesktopKey::Space, DesktopKey::R, DesktopKey::Escape, DesktopKey::Digit3}));
    Check(pad.buttons ==
              (gears::kPadA | gears::kPadRightShoulder | gears::kPadStart | gears::kPadDpadDown),
          "Space, R, Escape, and 3 did not map to A, RB, Start, and d-pad down");
    Check(MapDesktopControls(Keys({DesktopKey::Z})).buttons == 0,
          "an unbound key pressed a button");

    DesktopSnapshot mouse;
    mouse.mouse_buttons =
        static_cast<std::uint8_t>((1U << static_cast<unsigned>(DesktopMouseButton::Left)) |
                                  (1U << static_cast<unsigned>(DesktopMouseButton::Middle)));
    gears::PadState fired = MapDesktopControls(mouse);
    Check(fired.rightTrigger == 255 && fired.leftTrigger == 0,
          "left click did not pull only the right trigger");
    Check(fired.buttons == gears::kPadRightThumb, "middle click did not click the right stick");
}

void MouseTravelAimsTheRightStick()
{
    DesktopSnapshot slow;
    slow.pointer_captured = true;
    slow.pointer_dx = 1;
    slow.pointer_dy = 2;
    gears::PadState aim = MapDesktopControls(slow);
    Check(aim.thumbRX == gears::titles::gears1::kRightThumbDeadZone + 1000,
          "one pixel of travel did not start just beyond the dead zone");
    Check(aim.thumbRY == -(gears::titles::gears1::kRightThumbDeadZone + 2000),
          "downward pointer travel did not pull the stick down");

    DesktopSnapshot fast;
    fast.pointer_captured = true;
    fast.pointer_dx = -100000;
    Check(MapDesktopControls(fast).thumbRX == -32767, "a fast flick was not clamped to the stick");
}

void WindowSamplerFeedsThePadUntilARemoteTakesIt()
{
    x360port::DesktopInputState desktop;
    gears::InitialiseInput(true);
    gears::SetHostPadSource(gears::titles::gears1::SampleDesktopControls, &desktop);
    desktop.SetKey(static_cast<std::uint8_t>(DesktopKey::E), true);
    gears::PollHostInput();
    std::uint32_t packet = 0;
    Check(gears::CurrentPad(packet).buttons == gears::kPadX,
          "the window's keyboard did not reach the pad");

    gears::PadState remote;
    remote.buttons = gears::kPadB;
    Check(gears::SetRemotePad(remote), "the remote pad did not activate");
    gears::PollHostInput();
    Check(gears::CurrentPad(packet).buttons == gears::kPadB,
          "the window's keyboard overwrote the remote pad");
    gears::DisconnectRemotePad();
    gears::PollHostInput();
    Check(gears::CurrentPad(packet).buttons == gears::kPadX,
          "the window's keyboard did not resume after the remote disconnected");
}

} // namespace

int main()
{
    IdleDesktopIsANeutralPad();
    MovementKeysDeflectTheLeftStick();
    KeysAndButtonsMapToThePad();
    MouseTravelAimsTheRightStick();
    WindowSamplerFeedsThePadUntilARemoteTakesIt();
    std::cout << "gears1 desktop controls: 5 cases passed\n";
}
