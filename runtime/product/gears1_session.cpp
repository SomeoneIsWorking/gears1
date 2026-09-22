#include "gears1_session.h"

#include "titles/gears1/audio_mix.h"
#include "titles/gears1/xam_input_provider.h"

namespace gears::product
{

x360port::SystemSessionConfig Gears1SessionConfig(const ProductOptions &options,
                                                  const std::filesystem::path &storage_root)
{
    bool offscreen = options.mode == ProductMode::Offscreen;
    x360port::SystemSessionConfig config;
    config.application_name = "Gears of War";
    config.title_path = options.image;
    config.storage_root = storage_root;
    config.expected_title_id = options.title_id;
    config.audio = offscreen ? x360port::SystemAudio::Silent : x360port::SystemAudio::Device;
    config.input = {.state = titles::gears1::ReadXamPad,
                    .capabilities = titles::gears1::ReadXamCapabilities,
                    .context = nullptr};
    config.host_input =
        offscreen ? x360port::SystemHostInput::None : x360port::SystemHostInput::Gamepads;
    config.overrides.push_back({.address = titles::gears1::kAudioMixAddress,
                                .handler = titles::gears1::ApplyNativeAudioMix,
                                .context = nullptr});
    return config;
}

} // namespace gears::product
