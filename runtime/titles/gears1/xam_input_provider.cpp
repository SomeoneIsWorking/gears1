#include "xam_input_provider.h"

#include "input.h"

namespace gears::titles::gears1
{
namespace
{

constexpr std::uint32_t kAnyUserMask = 0xFFU;

} // namespace

x360port::XamPadSnapshot ReadXamPad(x360port::XamInputRequest request, void *) noexcept
{
    UpdateScriptedInput();
    if (request.user_index != 0U && (request.user_index & kAnyUserMask) != kAnyUserMask)
    {
        return {};
    }
    const PadSnapshot snapshot = ReadPadSnapshot();
    if (!snapshot.connected)
    {
        return {};
    }
    const PadState &pad = snapshot.state;
    return {.connected = true,
            .packet_number = snapshot.packet,
            .gamepad = {.buttons = pad.buttons,
                        .left_trigger = pad.leftTrigger,
                        .right_trigger = pad.rightTrigger,
                        .thumb_lx = pad.thumbLX,
                        .thumb_ly = pad.thumbLY,
                        .thumb_rx = pad.thumbRX,
                        .thumb_ry = pad.thumbRY}};
}

x360port::XamPadCapabilities ReadXamCapabilities(x360port::XamInputRequest request,
                                                 void *context) noexcept
{
    if (!ReadXamPad(request, context).connected)
    {
        return {};
    }
    // The virtual pad accepts every reported button and the full trigger/stick
    // ranges. There is no host vibration output, so motors are absent.
    return {.connected = true,
            .type = 1U,
            .sub_type = 1U,
            .flags = 0U,
            .supported_gamepad = {.buttons = 0xFFFFU,
                                  .left_trigger = 0xFFU,
                                  .right_trigger = 0xFFU,
                                  .thumb_lx = -1,
                                  .thumb_ly = -1,
                                  .thumb_rx = -1,
                                  .thumb_ry = -1}};
}

} // namespace gears::titles::gears1
