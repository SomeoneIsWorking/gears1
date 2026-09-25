#include "gears1_session.h"

#include "titles/gears1/audio_mix.h"
#include "titles/gears1/presentation.h"
#include "titles/gears1/xam_input_provider.h"

namespace gears::product
{

x360port::SystemSessionConfig
Gears1SessionConfig(const ProductOptions &options, const std::filesystem::path &storage_root,
                    x360port::DesktopInputState *desktop,
                    titles::gears1::AudioMixDifferential *audio_mix_check)
{
    bool offscreen = options.mode == ProductMode::Offscreen;
    x360port::SystemSessionConfig config;
    config.application_name = "Gears of War";
    config.title_path = options.image;
    config.storage_root = storage_root;
    config.expected_title_id = options.title_id;
    config.player_gamertag = kLocalPlayerGamertag;
    config.audio = offscreen ? x360port::SystemAudio::Silent : x360port::SystemAudio::Device;
    config.input = {.state = titles::gears1::ReadXamPad,
                    .capabilities = titles::gears1::ReadXamCapabilities,
                    .context = nullptr};
    config.host_input =
        offscreen ? x360port::SystemHostInput::None : x360port::SystemHostInput::Gamepads;
    config.desktop_input = desktop;
    if (options.console_pacing)
    {
        config.display_refresh_hz = x360port::kConsoleDisplayRefreshHz;
        config.max_presents_per_second = 0;
    }
    else
    {
        config.display_refresh_hz = titles::gears1::kDisplayRefreshHz;
        config.max_presents_per_second = titles::gears1::kTargetPresentsPerSecond;
    }
    config.write_perf_map = options.perf_map;
    if (audio_mix_check != nullptr)
    {
        config.overrides.push_back({.address = titles::gears1::kAudioMixAddress,
                                    .handler = titles::gears1::AudioMixDifferential::Apply,
                                    .context = audio_mix_check});
    }
    else
    {
        config.overrides.push_back({.address = titles::gears1::kAudioMixAddress,
                                    .handler = titles::gears1::ApplyNativeAudioMix,
                                    .context = nullptr});
    }
    return config;
}

} // namespace gears::product
