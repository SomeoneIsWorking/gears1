#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <lucent/config.h>
#include <lucent/log.h>
#include <lucent/platform.h>
#include <x360port/system_session.hpp>

#include "control_channel.h"
#include "gears1_session.h"
#include "input.h"
#include "offscreen_run.h"
#include "product_options.h"
#include "titles/gears1/desktop_controls.h"

namespace
{

using gears::product::ProductMode;
using gears::product::ProductOptions;

constexpr std::string_view kUserDataApplication = "GearsUE3";
constexpr std::string_view kSaveNamespace = "gears1";

// The player's per-application user-data directory, or the maintainer's
// explicit offscreen root.
[[nodiscard]] std::optional<std::filesystem::path> StorageRoot(const ProductOptions &options)
{
    if (options.mode == ProductMode::Offscreen)
    {
        return options.storage_root;
    }
    std::optional<std::filesystem::path> root =
        lucent::platform::user_data_directory(kUserDataApplication);
    if (!root)
    {
        return std::nullopt;
    }
    return *root / kSaveNamespace;
}

[[nodiscard]] int RunOffscreenProduct(const ProductOptions &options,
                                      const std::filesystem::path &storage_root)
{
    x360port::SystemSessionCreateResult created = x360port::SystemSession::CreateOffscreen(
        gears::product::Gears1SessionConfig(options, storage_root, nullptr));
    if (!created)
    {
        lucent::error("product", "the console could not be composed: {}", created.failure.detail);
        return EXIT_FAILURE;
    }
    if (x360port::RuntimeFailure failure = created.session->Launch())
    {
        lucent::error("product", "Gears of War did not launch: {}", failure.detail);
        x360port::SystemSession::EndProcess(EXIT_FAILURE);
    }
    std::optional<gears::product::ControlChannel> control;
    if (options.control_port != 0)
    {
        control.emplace(*created.session, options.control_port);
        if (!control->Start())
        {
            lucent::error("product", "the control channel cannot serve on loopback port {}",
                          options.control_port);
            x360port::SystemSession::EndProcess(EXIT_FAILURE);
        }
    }
    bool evidence = gears::product::RunOffscreen(*created.session, options);
    x360port::SystemSession::EndProcess(evidence ? EXIT_SUCCESS : EXIT_FAILURE);
}

} // namespace

int main(int argc, char **argv)
{
    lucent::config::set_prefix("GEARS_");
    gears::product::ProductOptionsResult parsed = gears::product::ParseProductOptions(
        std::span<const char *const>(argv + 1, static_cast<std::size_t>(argc - 1)));
    if (!parsed)
    {
        lucent::error("product", "{}", parsed.error);
        return EXIT_FAILURE;
    }
    const ProductOptions &options = parsed.options;
    std::optional<std::filesystem::path> storage_root = StorageRoot(options);
    if (!storage_root)
    {
        lucent::error("product", "no per-user data directory is available for saves");
        return EXIT_FAILURE;
    }
    // The title's pad carries scripted, remote, and keyboard-and-mouse input;
    // host gamepads are the console's, merged with it by the session.
    if (options.mode == ProductMode::Offscreen)
    {
        gears::InitialiseInput(false);
        return RunOffscreenProduct(options, *storage_root);
    }
    x360port::DesktopInputState desktop;
    gears::InitialiseInput(true);
    gears::SetHostPadSource(gears::titles::gears1::SampleDesktopControls, &desktop);
    x360port::RuntimeFailure failure = x360port::RunWindowedSystem(
        gears::product::Gears1SessionConfig(options, *storage_root, &desktop));
    lucent::error("product", "Gears of War could not start: {}", failure.detail);
    return EXIT_FAILURE;
}
