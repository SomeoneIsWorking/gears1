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
            .buttons = pad.buttons,
            .left_trigger = pad.leftTrigger,
            .right_trigger = pad.rightTrigger,
            .thumb_lx = pad.thumbLX,
            .thumb_ly = pad.thumbLY,
            .thumb_rx = pad.thumbRX,
            .thumb_ry = pad.thumbRY};
}

} // namespace gears::titles::gears1
