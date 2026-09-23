#include "product_options.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <system_error>

namespace gears::product
{
namespace
{

template <typename Integer>
[[nodiscard]] bool ParseInteger(std::string_view text, int base, Integer &value)
{
    const char *end = text.data() + text.size();
    auto [consumed, error] = std::from_chars(text.data(), end, value, base);
    return !text.empty() && error == std::errc{} && consumed == end;
}

class ArgumentCursor final
{
  public:
    explicit ArgumentCursor(std::span<const char *const> arguments) noexcept : arguments_(arguments)
    {
    }

    [[nodiscard]] bool Done() const noexcept { return index_ >= arguments_.size(); }

    [[nodiscard]] std::string_view Next() noexcept { return arguments_[index_++]; }

    // The value after an option, or false when the option ends the line.
    [[nodiscard]] bool Value(std::string_view &value) noexcept
    {
        if (Done())
        {
            return false;
        }
        value = Next();
        return true;
    }

  private:
    std::span<const char *const> arguments_;
    std::size_t index_ = 0;
};

constexpr std::array<std::string_view, 7> kValueOptions = {
    "--image",       "--title-id",      "--storage-root", "--seconds",
    "--capture-dir", "--capture-every", "--control-port"};

[[nodiscard]] bool TakesValue(std::string_view option) noexcept
{
    return std::ranges::find(kValueOptions, option) != kValueOptions.end();
}

[[nodiscard]] std::string ValidateOptions(const ProductOptions &options)
{
    if (options.image.empty())
    {
        return "--image is required";
    }
    if (options.title_id == 0)
    {
        return "--title-id is required";
    }
    if (options.mode == ProductMode::Window)
    {
        if (!options.storage_root.empty() || options.run_seconds != 0 ||
            !options.capture_directory.empty() || options.capture_interval_seconds != 0 ||
            options.perf_map || options.control_port != 0)
        {
            return "--storage-root, --seconds, --capture-dir, --capture-every, --perf-map, and "
                   "--control-port apply only with --offscreen";
        }
        return {};
    }
    if (options.storage_root.empty() || !options.storage_root.is_absolute())
    {
        return "--offscreen requires an absolute --storage-root";
    }
    if (options.run_seconds == 0)
    {
        return "--offscreen requires a positive --seconds";
    }
    if (options.capture_directory.empty() != (options.capture_interval_seconds == 0))
    {
        return "--capture-dir and --capture-every are given together";
    }
    return {};
}

} // namespace

ProductOptionsResult ParseProductOptions(std::span<const char *const> arguments)
{
    ProductOptionsResult result;
    ProductOptions &options = result.options;
    ArgumentCursor cursor(arguments);
    while (!cursor.Done())
    {
        std::string_view option = cursor.Next();
        if (option == "--offscreen")
        {
            options.mode = ProductMode::Offscreen;
            continue;
        }
        if (option == "--perf-map")
        {
            options.perf_map = true;
            continue;
        }
        if (!TakesValue(option))
        {
            result.error = "unknown option " + std::string(option);
            return result;
        }
        std::string_view value;
        if (!cursor.Value(value))
        {
            result.error = std::string(option) + " requires a value";
            return result;
        }
        bool parsed = true;
        if (option == "--image")
        {
            options.image = value;
        }
        else if (option == "--title-id")
        {
            parsed = ParseInteger(value, 16, options.title_id);
        }
        else if (option == "--storage-root")
        {
            options.storage_root = value;
        }
        else if (option == "--seconds")
        {
            parsed = ParseInteger(value, 10, options.run_seconds);
        }
        else if (option == "--capture-dir")
        {
            options.capture_directory = value;
        }
        else if (option == "--control-port")
        {
            parsed = ParseInteger(value, 10, options.control_port) && options.control_port != 0;
        }
        else
        {
            parsed = ParseInteger(value, 10, options.capture_interval_seconds);
        }
        if (!parsed)
        {
            result.error = std::string(option) + " has a malformed value: " + std::string(value);
            return result;
        }
    }
    result.error = ValidateOptions(options);
    return result;
}

} // namespace gears::product
