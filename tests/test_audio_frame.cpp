// Tests for the render-driver frame's layout.
//
// The title hands XAudioSubmitRenderDriverFrame one frame of audio as SIX PLANES
// of 256 big-endian floats -- channel-major, all of channel 0, then all of
// channel 1, and so on. Every host audio API wants the opposite: interleaved
// frames, one sample of each channel in turn.
//
// Reading a planar buffer as an interleaved one does not fail, it plays: each
// output "channel" walks one plane at six times the rate and falls into the next
// plane every 256/6 samples. What comes out is the same audio an octave and a
// half too high with a click every 43 samples, which is what the port sounded
// like -- so the property under test is exactly which of the two readings the
// runtime performs, on a frame built so the two cannot agree.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "audio_frame.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

constexpr uint32_t kChannels = 6;
constexpr uint32_t kSamples = 256;

void StoreBigEndianFloat(uint8_t* at, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    at[0] = uint8_t(bits >> 24);
    at[1] = uint8_t(bits >> 16);
    at[2] = uint8_t(bits >> 8);
    at[3] = uint8_t(bits);
}

// Channel c's plane holds nothing but the value c. Interleaved output must then
// repeat 0,1,2,3,4,5 for every frame; the interleaved READING of the same bytes
// gives 0,0,0,... for the first 42 output frames instead, so no test written this
// way can pass by accident.
std::vector<uint8_t> PlanarFrameWithChannelConstants()
{
    std::vector<uint8_t> guest(kChannels * kSamples * 4);
    for (uint32_t c = 0; c < kChannels; ++c)
        for (uint32_t s = 0; s < kSamples; ++s)
            StoreBigEndianFloat(&guest[(c * kSamples + s) * 4], float(c));
    return guest;
}

void TestPlanarGuestFrameBecomesInterleavedHostFrame()
{
    const std::vector<uint8_t> guest = PlanarFrameWithChannelConstants();
    std::vector<float> host(kChannels * kSamples, -1.0f);
    gears::DeinterleaveGuestAudioFrame(guest.data(), host.data(), kChannels, kSamples);

    bool ok = true;
    for (uint32_t s = 0; s < kSamples && ok; ++s)
        for (uint32_t c = 0; c < kChannels; ++c)
            if (host[s * kChannels + c] != float(c))
            {
                printf("  at sample %u channel %u: got %g, want %g\n", s, c,
                       double(host[s * kChannels + c]), double(c));
                ok = false;
                break;
            }
    Check(ok, "a channel-major guest frame comes out interleaved, one sample of"
              " each channel in turn");
}

// The bug had a shape, and this pins it: with the wrong reading, the first host
// frame is six copies of channel 0's first sample.
void TestTheInterleavedMisreadingIsRejected()
{
    const std::vector<uint8_t> guest = PlanarFrameWithChannelConstants();
    std::vector<float> host(kChannels * kSamples, -1.0f);
    gears::DeinterleaveGuestAudioFrame(guest.data(), host.data(), kChannels, kSamples);
    bool allZero = true;
    for (uint32_t c = 0; c < kChannels; ++c)
        if (host[c] != 0.0f)
            allZero = false;
    Check(!allZero, "the first host frame is NOT six copies of channel 0 -- that is"
                    " the planar buffer being read as interleaved");
}

void TestByteOrderIsTheGuestsBigEndian()
{
    std::vector<uint8_t> guest(kChannels * kSamples * 4, 0);
    StoreBigEndianFloat(&guest[0], 0.5f);                       // ch0 sample 0
    StoreBigEndianFloat(&guest[(1 * kSamples) * 4], -0.25f);    // ch1 sample 0
    std::vector<float> host(kChannels * kSamples, 9.0f);
    gears::DeinterleaveGuestAudioFrame(guest.data(), host.data(), kChannels, kSamples);
    Check(host[0] == 0.5f, "a big-endian guest float arrives as its host value");
    Check(host[1] == -0.25f, "including a negative one from the next plane");
}

void TestStereoGeometryIsNotHardcodedToSix()
{
    constexpr uint32_t kCh = 2, kN = 4;
    std::vector<uint8_t> guest(kCh * kN * 4);
    for (uint32_t c = 0; c < kCh; ++c)
        for (uint32_t s = 0; s < kN; ++s)
            StoreBigEndianFloat(&guest[(c * kN + s) * 4], float(c * 10 + s));
    std::vector<float> host(kCh * kN, -1.0f);
    gears::DeinterleaveGuestAudioFrame(guest.data(), host.data(), kCh, kN);
    const float want[8] = {0, 10, 1, 11, 2, 12, 3, 13};
    bool ok = true;
    for (uint32_t i = 0; i < 8; ++i)
        ok = ok && host[i] == want[i];
    Check(ok, "the conversion follows the geometry it is given, not a baked 6x256");
}

} // namespace

int main()
{
    TestPlanarGuestFrameBecomesInterleavedHostFrame();
    TestTheInterleavedMisreadingIsRejected();
    TestByteOrderIsTheGuestsBigEndian();
    TestStereoGeometryIsNotHardcodedToSix();
    if (g_failures != 0)
    {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("audio frame layout: all checks passed\n");
    return 0;
}
