#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

#include "title_executable.h"

int main()
{
    gears::LoadedTitleExecutable loaded;
    loaded.identity.imageSize = 7;
    std::string error;
    constexpr std::array<std::uint8_t, 4> malformed{'X', 'E', 'X', '2'};
    if (gears::LoadTitleExecutable(malformed, loaded, error))
    {
        std::fputs("FAIL: truncated XEX was accepted\n", stderr);
        return 1;
    }
    if (error.empty())
    {
        std::fputs("FAIL: checked loader did not explain refusal\n", stderr);
        return 1;
    }
    if (loaded.image.data != nullptr || loaded.identity.imageSize != 0)
    {
        std::fputs("FAIL: failed load published partial executable state\n", stderr);
        return 1;
    }
    std::puts("all title executable tests passed");
    return 0;
}
