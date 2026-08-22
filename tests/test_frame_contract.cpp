#include <cstdio>

#include "frame_contract.h"

namespace
{

void Check(gears::FrameTransition actual, gears::FrameTransition expected, const char *name,
           int &failures)
{
    if (actual == expected)
        return;
    std::printf("FAIL %s: got %u, expected %u\n", name, static_cast<unsigned>(actual),
                static_cast<unsigned>(expected));
    ++failures;
}

void Check(uint64_t actual, uint64_t expected, const char *name, int &failures)
{
    if (actual == expected)
        return;
    std::printf("FAIL %s: got %llu, expected %llu\n", name, static_cast<unsigned long long>(actual),
                static_cast<unsigned long long>(expected));
    ++failures;
}

} // namespace

int main()
{
    using gears::FrameId;
    using gears::FrameTransition;

    int failures = 0;
    gears::FrameContract contract;

    Check(contract.Publish(FrameId{}), FrameTransition::kRejectedInvalid,
          "zero cannot identify a published frame", failures);
    Check(contract.Present(FrameId{}), FrameTransition::kRejectedInvalid,
          "zero cannot identify a presented frame", failures);
    Check(contract.Present(FrameId{1}), FrameTransition::kRejectedUnpublished,
          "a frame cannot be presented before publication", failures);

    Check(contract.Publish(FrameId{3}), FrameTransition::kAdvanced,
          "the first publication establishes identity", failures);
    Check(contract.Publish(FrameId{3}), FrameTransition::kRejectedDuplicate,
          "a duplicate publication is rejected", failures);
    Check(contract.Publish(FrameId{2}), FrameTransition::kRejectedRegression,
          "publication cannot move backward", failures);

    Check(contract.Present(FrameId{4}), FrameTransition::kRejectedUnpublished,
          "presentation cannot outrun publication", failures);
    Check(contract.Present(FrameId{2}), FrameTransition::kRejectedStale,
          "presentation cannot select an older identity", failures);
    Check(contract.Present(FrameId{3}), FrameTransition::kAdvanced,
          "the latest published frame may be presented", failures);
    Check(contract.Present(FrameId{3}), FrameTransition::kRepeated,
          "the display may intentionally hold the latest frame", failures);

    Check(contract.Publish(FrameId{7}), FrameTransition::kAdvanced,
          "publication may skip dropped frame identities", failures);
    Check(contract.Present(FrameId{3}), FrameTransition::kRejectedStale,
          "a previously valid frame becomes stale after a newer publication", failures);
    Check(contract.Present(FrameId{7}), FrameTransition::kAdvanced,
          "presentation advances to the new identity", failures);

    const gears::FrameContractSnapshot snapshot = contract.Snapshot();
    Check(snapshot.published.value, 7, "rejections do not change the published identity", failures);
    Check(snapshot.presented.value, 7, "rejections do not change the presented identity", failures);

    if (failures == 0)
        std::puts("frame contract tests passed");
    return failures != 0;
}
