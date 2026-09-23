#include "pad_form.h"

#include <charconv>
#include <cstdint>
#include <format>
#include <system_error>
#include <vector>

#include <lucent/http.h>

namespace gears::product
{
namespace
{

constexpr std::int16_t kStickLimit = 32767;
constexpr std::uint8_t kTriggerLimit = 255;

template <typename Integer>
[[nodiscard]] bool ParseBounded(std::string_view text, Integer minimum, Integer maximum,
                                Integer &value)
{
    std::int64_t parsed = 0;
    const char *end = text.data() + text.size();
    auto [consumed, error] = std::from_chars(text.data(), end, parsed);
    if (text.empty() || error != std::errc{} || consumed != end || parsed < minimum ||
        parsed > maximum)
    {
        return false;
    }
    value = static_cast<Integer>(parsed);
    return true;
}

// A comma-separated list of button names; empty for no buttons.
[[nodiscard]] bool ParseButtons(std::string_view text, std::uint16_t &buttons)
{
    buttons = 0;
    if (text.empty())
    {
        return true;
    }
    while (true)
    {
        std::size_t separator = text.find(',');
        std::uint16_t button = 0;
        if (!PadButtonByName(text.substr(0, separator), button))
        {
            return false;
        }
        buttons |= button;
        if (separator == std::string_view::npos)
        {
            return true;
        }
        text.remove_prefix(separator + 1);
    }
}

[[nodiscard]] bool ParseStick(std::string_view text, std::int16_t &value)
{
    return ParseBounded(text, static_cast<std::int16_t>(-kStickLimit), kStickLimit, value);
}

[[nodiscard]] bool ParseTrigger(std::string_view text, std::uint8_t &value)
{
    return ParseBounded(text, std::uint8_t{0}, kTriggerLimit, value);
}

[[nodiscard]] bool ParseField(const lucent::http::FormField &field, PadState &pad,
                              std::string &error)
{
    bool valid = false;
    if (field.name == "buttons")
    {
        valid = ParseButtons(field.value, pad.buttons);
    }
    else if (field.name == "lx")
    {
        valid = ParseStick(field.value, pad.thumbLX);
    }
    else if (field.name == "ly")
    {
        valid = ParseStick(field.value, pad.thumbLY);
    }
    else if (field.name == "rx")
    {
        valid = ParseStick(field.value, pad.thumbRX);
    }
    else if (field.name == "ry")
    {
        valid = ParseStick(field.value, pad.thumbRY);
    }
    else if (field.name == "lt")
    {
        valid = ParseTrigger(field.value, pad.leftTrigger);
    }
    else if (field.name == "rt")
    {
        valid = ParseTrigger(field.value, pad.rightTrigger);
    }
    else
    {
        error = std::format("unknown pad field '{}'", field.name);
        return false;
    }
    if (!valid)
    {
        error = std::format("invalid value '{}' for {}", field.value, field.name);
    }
    return valid;
}

} // namespace

bool ParsePadForm(std::string_view encoded, PadState &pad, std::string &error)
{
    pad = {};
    std::vector<lucent::http::FormField> fields;
    if (!lucent::http::parse_form_urlencoded(encoded, fields, error))
    {
        return false;
    }
    for (const lucent::http::FormField &field : fields)
    {
        if (!ParseField(field, pad, error))
        {
            return false;
        }
    }
    return true;
}

} // namespace gears::product
