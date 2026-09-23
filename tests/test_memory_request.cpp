#include "memory_request.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

using gears::product::MemoryRequest;
using gears::product::ParseMemoryRequest;

void Require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "memory request: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void RequireRefused(std::string_view fields, std::string_view expected)
{
    MemoryRequest request;
    std::string error;
    Require(!ParseMemoryRequest(fields, request, error),
            "accepted invalid fields: " + std::string(fields));
    Require(error.find(expected) != std::string::npos,
            "refusal '" + error + "' does not name: " + std::string(expected));
}

} // namespace

int main()
{
    MemoryRequest request;
    std::string error;
    Require(ParseMemoryRequest("address=0x82BED138&length=4", request, error), error);
    Require(request.address == 0x82BED138U && request.length == 4U, "fields were not read");
    Require(ParseMemoryRequest("length=4096&address=fffff000", request, error), error);
    Require(request.address == 0xFFFFF000U && request.length == 4096U,
            "an unprefixed address or the last page was not read");

    RequireRefused("address=0x82BED138", "length is required");
    RequireRefused("length=4", "address is required");
    RequireRefused("address=0x&length=4", "address");
    RequireRefused("address=0x1G&length=4", "address");
    RequireRefused("address=0x100000000&length=4", "address");
    RequireRefused("address=0x82000000&length=0", "length");
    RequireRefused("address=0x82000000&length=4097", "length");
    RequireRefused("address=0xFFFFFFFF&length=2", "passes the top");
    RequireRefused("address=0x82000000&length=4&typo=1", "typo");
    return EXIT_SUCCESS;
}
