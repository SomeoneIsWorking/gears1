#include "input.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include <lucent/config.h>

// Run with GEARS_INPUT_SCRIPT="f1:A,f2:" (the CTest registration sets it): a
// script that holds A at guest frame 1 and releases it at frame 2.
namespace
{

std::uint64_t g_frame = 0;

std::uint64_t CurrentFrame()
{
    return g_frame;
}

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "script handoff: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main()
{
    lucent::config::set_prefix("GEARS_");
    Require(lucent::config::text("INPUT_SCRIPT") == "f1:A,f2:",
            "GEARS_INPUT_SCRIPT is not the script this test drives");
    gears::SetGuestFrameSource(CurrentFrame);
    gears::InitialiseInput(false);

    gears::PadState remote;
    remote.thumbLY = 32767;
    Require(!gears::SetRemotePad(remote), "a remote write was accepted before the script ran");
    Require(gears::CurrentInputSource() == gears::InputSource::kScript,
            "the script does not own the pad before it runs");

    g_frame = 1;
    gears::PollHostInput();
    uint32_t packet = 0;
    Require(gears::CurrentPad(packet).buttons == gears::kPadA, "the scripted A was not applied");
    Require(!gears::SetRemotePad(remote), "a remote write was accepted with a step still to fire");

    g_frame = 2;
    gears::PollHostInput();
    Require(gears::CurrentPad(packet) == gears::PadState{}, "the scripted release was not applied");
    Require(gears::SetRemotePad(remote), "the finished script kept the pad");
    Require(gears::CurrentInputSource() == gears::InputSource::kRemote,
            "the remote controller does not own the pad after the script");
    gears::PollHostInput();
    Require(gears::CurrentPad(packet) == remote, "the finished script overwrote the remote pad");
    std::cout << "script handoff: the pad passed to the remote controller after the last step\n";
    return EXIT_SUCCESS;
}
