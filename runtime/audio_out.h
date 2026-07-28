#pragma once

#include <cstdint>

namespace gears
{

// The host audio device: the last step between the title's mix and a speaker.
//
// Everything upstream of this is now real -- the pump asks the title for a
// frame, the title mixes one (decoding XMA to do it), and the frame arrives
// here. This plays it.
//
// Deliberately the LAST thing built. A device fed silence and a device fed
// nothing sound identical, so wiring one before the mix was proven would have
// made every upstream failure invisible.
bool OpenAudioOutput(uint32_t channels, uint32_t sampleRate);

// One frame of interleaved float samples, host byte order.
void PlayAudioFrame(const float* samples, uint32_t framesPerChannel);

void CloseAudioOutput();

} // namespace gears
