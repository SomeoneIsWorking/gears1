#include "audio_out.h"

#include <atomic>

#include <lucent/log.h>

#if GEARS_HAVE_PRESENTER
#include <SDL3/SDL.h>
#endif

namespace gears
{

#if !GEARS_HAVE_PRESENTER

bool OpenAudioOutput(uint32_t, uint32_t)
{
    lucent::warn("audio", "this build has no SDL, so there is no audio device");
    return false;
}
void PlayAudioFrame(const float*, uint32_t) {}
void CloseAudioOutput() {}

#else

namespace
{
SDL_AudioStream* g_stream = nullptr;
std::atomic<uint64_t> g_framesPlayed{0};
std::atomic<uint64_t> g_starvedFrames{0};
uint32_t g_channels = 0;
uint32_t g_bytesPerFrame = 0;
} // namespace

bool OpenAudioOutput(uint32_t channels, uint32_t sampleRate)
{
    if (g_stream)
        return true;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
    {
        // Not fatal. A machine with no sound server still runs the game, and
        // the mix is still measurable through GEARS_AUDIO_WAV.
        lucent::warn("audio", "SDL_InitSubSystem(AUDIO) failed: {} -- no device,"
            " the title still mixes", SDL_GetError());
        return false;
    }

    // The source format is the title's, verbatim. SDL converts to whatever the
    // device wants, including downmixing 5.1 to stereo, which is a conversion
    // worth letting a library own rather than writing one here.
    const SDL_AudioSpec spec{SDL_AUDIO_F32, int(channels), int(sampleRate)};
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                         nullptr, nullptr);
    if (!g_stream)
    {
        lucent::warn("audio", "no playback device: {} -- the title still mixes",
                     SDL_GetError());
        return false;
    }

    g_channels = channels;
    g_bytesPerFrame = channels * uint32_t(sizeof(float));
    SDL_ResumeAudioStreamDevice(g_stream);
    lucent::info("audio", "playback device open: {} channels at {} Hz", channels,
                 sampleRate);
    return true;
}

void PlayAudioFrame(const float* samples, uint32_t framesPerChannel)
{
    if (!g_stream || !samples || framesPerChannel == 0)
        return;

    // How much is still queued BEFORE this frame goes in. The title produces
    // frames on its own clock and the device consumes on the hardware's, so
    // the queue draining to nothing is the device having nothing to play --
    // an audible gap. Counting it is the difference between "audio works" and
    // "audio works when the machine is not busy", which is a real distinction
    // here: the pump has been measured falling behind under a CPU-bound
    // guest.
    if (SDL_GetAudioStreamQueued(g_stream) == 0 && g_framesPlayed.load() > 0)
        g_starvedFrames.fetch_add(1);

    if (!SDL_PutAudioStreamData(g_stream, samples, int(framesPerChannel * g_bytesPerFrame)))
    {
        lucent::warn("audio", "SDL_PutAudioStreamData failed: {}", SDL_GetError());
        return;
    }

    const uint64_t played = g_framesPlayed.fetch_add(1) + 1;
    if (played % 2000 == 0)
    {
        const uint64_t starved = g_starvedFrames.load();
        lucent::info("audio", "{} frames played, {} arrived with the device"
            " already empty ({:.1f}%)", played, starved,
            100.0 * double(starved) / double(played));
    }
}

void CloseAudioOutput()
{
    if (!g_stream)
        return;
    SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
}

#endif

} // namespace gears
