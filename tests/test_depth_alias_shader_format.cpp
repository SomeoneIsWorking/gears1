#include <cstdint>
#include <cstdio>
#include <vector>

#include "edram_depth_alias_spv.h"

int main()
{
    const std::vector<uint32_t> &words = gears::native::EdramDepthAliasSpirv();
    bool foundUnformattedStorageImage = false;
    int failures = 0;

    for (size_t at = 5; at < words.size();)
    {
        const uint32_t instruction = words[at];
        const uint16_t wordCount = uint16_t(instruction >> 16);
        const uint16_t opcode = uint16_t(instruction);
        if (wordCount == 0 || at + wordCount > words.size())
        {
            std::puts("FAIL: malformed generated SPIR-V instruction stream");
            return 1;
        }
        // OpTypeImage has nine words. Operand 6 is Sampled (2 means storage),
        // and operand 7 is Image Format (0 means Unknown/unformatted).
        if (opcode == 25 && wordCount == 9 && words[at + 7] == 2)
        {
            if (words[at + 8] == 0)
                foundUnformattedStorageImage = true;
            else
            {
                std::printf("FAIL: storage OpTypeImage declares format %u\n", words[at + 8]);
                ++failures;
            }
        }
        at += wordCount;
    }

    if (!foundUnformattedStorageImage)
    {
        std::puts("FAIL: generated depth-alias shader has no unformatted storage image");
        ++failures;
    }
    if (failures == 0)
        std::puts("depth alias shader format test passed");
    return failures != 0;
}
