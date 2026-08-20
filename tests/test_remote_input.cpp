#include "input.h"

#include <cstdlib>
#include <iostream>

namespace
{

void Check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    uint16_t button = 0;
    Check(gears::PadButtonByName("A", button) && button == gears::kPadA,
          "the shared button table resolves A");
    Check(gears::PadButtonByName("START", button) && button == gears::kPadStart,
          "the shared button table resolves START");
    Check(!gears::PadButtonByName("TYPO", button), "the shared button table rejects unknown names");

    Check(!gears::PadConnected(), "no source reports disconnected before remote activation");
    gears::PadState commanded;
    commanded.buttons = gears::kPadA | gears::kPadStart;
    commanded.thumbLY = 12345;
    Check(gears::SetRemotePad(commanded), "remote input activates without a script");
    Check(gears::PadConnected(), "remote activation connects the pad");
    Check(gears::CurrentInputSource() == gears::InputSource::kRemote,
          "remote activation owns the pad");

    uint32_t packet = 0;
    Check(gears::CurrentPad(packet) == commanded && packet == 1,
          "remote input publishes one atomic packet");
    Check(gears::SetRemotePad(commanded), "repeating a remote state succeeds");
    gears::CurrentPad(packet);
    Check(packet == 1, "an unchanged state does not invent a packet edge");

    gears::ReleaseRemotePad();
    Check(gears::CurrentPad(packet) == gears::PadState{} && packet == 2,
          "release publishes a neutral packet");
    Check(gears::PadConnected(), "release keeps the remote controller connected");

    gears::DisconnectRemotePad();
    Check(!gears::PadConnected(), "disconnect removes the remote controller source");
    Check(gears::CurrentInputSource() == gears::InputSource::kNone,
          "disconnect returns input ownership to none");
    std::cout << "remote input tests passed\n";
}
