#include "memory_request.h"

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

[[nodiscard]] bool ParseAddress(std::string_view text, std::uint32_t &address)
{
    if (text.starts_with("0x") || text.starts_with("0X"))
    {
        text.remove_prefix(2);
    }
    const char *end = text.data() + text.size();
    auto [consumed, error] = std::from_chars(text.data(), end, address, 16);
    return !text.empty() && error == std::errc{} && consumed == end;
}

[[nodiscard]] bool ParseLength(std::string_view text, std::uint32_t &length)
{
    const char *end = text.data() + text.size();
    auto [consumed, error] = std::from_chars(text.data(), end, length);
    return !text.empty() && error == std::errc{} && consumed == end && length >= 1 &&
           length <= kMaxMemoryReadBytes;
}

} // namespace

bool ParseMemoryRequest(std::string_view encoded, MemoryRequest &request, std::string &error)
{
    request = {};
    std::vector<lucent::http::FormField> fields;
    if (!lucent::http::parse_form_urlencoded(encoded, fields, error))
    {
        return false;
    }
    bool has_address = false;
    bool has_length = false;
    for (const lucent::http::FormField &field : fields)
    {
        if (field.name == "address")
        {
            has_address = ParseAddress(field.value, request.address);
            if (!has_address)
            {
                error =
                    std::format("address '{}' is not a 32-bit hexadecimal address", field.value);
                return false;
            }
        }
        else if (field.name == "length")
        {
            has_length = ParseLength(field.value, request.length);
            if (!has_length)
            {
                error = std::format("length '{}' is not a byte count in 1..{}", field.value,
                                    kMaxMemoryReadBytes);
                return false;
            }
        }
        else
        {
            error = std::format("unknown field '{}'", field.name);
            return false;
        }
    }
    if (!has_address || !has_length)
    {
        error = has_address ? "length is required" : "address is required";
        return false;
    }
    if (static_cast<std::uint64_t>(request.address) + request.length > 0x1'0000'0000ULL)
    {
        error = std::format("length {} from address 0x{:08X} passes the top of guest memory",
                            request.length, request.address);
        return false;
    }
    return true;
}

} // namespace gears::product
