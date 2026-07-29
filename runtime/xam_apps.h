// The Xam applications a title talks to through XMsgInProcessCall and
// XMsgStartIORequest.
//
// These are separate system processes on the console -- the media player, the
// gaming/presence service, the Live client -- and a title reaches them by
// posting a numbered message with a buffer. The runtime used to refuse every
// one of them. That is honest for a single message, but collectively it means
// the title is talking to a system that does not exist, and it cannot tell the
// difference between "this console has no Live" and "this message went
// nowhere".
//
// The distinction this header exists to preserve: a message that is HANDLED
// (even if the answer is "not logged on") is different from one that is
// UNKNOWN. Unknown ones stay loud, so an unimplemented service is still
// visible rather than being buried under a blanket success.
#pragma once

#include <cstdint>

namespace gears
{

// Application ids, as the console numbers them.
constexpr uint32_t kXamAppXmp = 0xFA;        // media / background music
constexpr uint32_t kXamAppXgi = 0xFB;        // gaming: contexts, properties
constexpr uint32_t kXamAppXLiveBase = 0xFC;  // Live client

// The console's status for a service that needs a signed-in Live account. This
// is a real answer a real console gives, and titles handle it; an empty
// success is not, and leaves them waiting.
constexpr uint32_t kOnlineNotLoggedOn = 0x80151802;

// Handles one message. Returns false if the app or message is unknown, in
// which case `status` is untouched and the caller should report it as
// unimplemented. Returns true when handled, with the console's status in
// `status` -- which may itself be a failure.
bool DispatchXamMessage(uint8_t* guestBase, uint32_t app, uint32_t message,
                        uint32_t bufferAddress, uint32_t bufferLength,
                        uint32_t& status);

} // namespace gears
