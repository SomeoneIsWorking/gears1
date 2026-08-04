// The layout of one render-driver audio frame, and the conversion out of it.
//
// XAudioSubmitRenderDriverFrame hands us a pointer to the frame the title just
// mixed. The frame is SIX PLANES of 256 big-endian floats -- all of channel 0,
// then all of channel 1, and so on -- which is the layout Xenia's converter reads
// (`input[channel * ch_sample_count + sample]` in
// extern/xenia/src/xenia/apu/conversion.h, whose name for it is "sequential_6").
// Every host audio API wants the other one: interleaved frames, one sample of
// each channel in turn.
//
// Reading the planar buffer as if it were interleaved does not fail, it PLAYS.
// Each output channel walks a single plane at six times the rate and falls into
// the next plane every 256/6 samples, so the audio comes out far too high with a
// click at every plane crossing. Measured in the mix the title submitted: 3.5% of
// samples were discontinuities, clustered at offsets 0, 43, 86, 128 and 171
// within each 256-sample frame -- multiples of 256/6 -- and 7.3% of the
// spectrum's energy sat above 20 kHz, which no music has.
//
// The conversion is a header-only pure function so the layout can be tested
// without a device, a guest or a thread: tests/test_audio_frame.cpp builds a
// frame whose planes hold per-channel constants, which the two readings cannot
// both satisfy.
#pragma once

#include <cstdint>
#include <cstring>

namespace gears
{

// `guest` is channels x samplesPerChannel big-endian floats, channel-major.
// `host` receives samplesPerChannel x channels host floats, interleaved.
inline void DeinterleaveGuestAudioFrame(const uint8_t* guest, float* host,
                                        uint32_t channels,
                                        uint32_t samplesPerChannel)
{
    for (uint32_t channel = 0; channel < channels; ++channel)
    {
        const uint8_t* plane = guest + size_t(channel) * samplesPerChannel * 4;
        for (uint32_t sample = 0; sample < samplesPerChannel; ++sample)
        {
            const uint8_t* at = plane + size_t(sample) * 4;
            const uint32_t bits = (uint32_t(at[0]) << 24) | (uint32_t(at[1]) << 16) |
                                  (uint32_t(at[2]) << 8) | uint32_t(at[3]);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            host[size_t(sample) * channels + channel] = value;
        }
    }
}

} // namespace gears
