// Tests for Vulkan queue-family selection, ahead of unifying the two devices.
//
// WHY THIS IS BEING FACTORED OUT. runtime/gpu_draw.cpp and runtime/gpu_present.cpp
// each create their own VkInstance and VkDevice, and each picks a queue family with
// its own inline loop. The consequence is not just duplication: because the drawn
// image lives on the draw device and the swapchain lives on the present device,
// every rendered frame is read BACK to host memory and then uploaded again through
// a staging buffer to be shown. Two devices means a full round trip per frame that
// one device would not need at all.
//
// Unifying them makes the selection harder, which is exactly why it is worth
// testing: the single family now has to satisfy BOTH sides -- graphics for the draw
// path, and presentation to the window's surface for the present path -- and the
// two files' existing loops each only checked their own half.
//
// The failure mode to guard against is a selector that cannot say no. If nothing
// qualifies, returning family 0 and hoping is how a port produces a device that
// silently cannot present, and the symptom surfaces later as a blank window with
// no error. So "none" is a value here, and the caller is expected to fail loudly
// on it.

#include <cstdio>
#include <vector>

#include "gpu_queue_family.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

using gears::QueueFamily;
using gears::ChooseQueueFamily;
using gears::kNoQueueFamily;

// The ordinary desktop case: one family that does everything.
void TestSingleCapableFamily()
{
    const std::vector<QueueFamily> families = {
        {/*graphics=*/true, /*present=*/true, /*count=*/1},
    };
    Check(ChooseQueueFamily(families, /*needPresent=*/true) == 0,
        "one family with graphics and present is chosen when both are needed");
    Check(ChooseQueueFamily(families, /*needPresent=*/false) == 0,
        "and also when only graphics is needed");
}

// A family that presents but cannot draw is useless to this runtime, and one that
// draws but cannot present is useless when a window exists. Both must be refused
// rather than accepted on the strength of the half they do satisfy -- which is
// precisely what the two separate inline loops each did.
void TestHalfCapableFamiliesAreRefused()
{
    const std::vector<QueueFamily> presentOnly = {{false, true, 1}};
    Check(ChooseQueueFamily(presentOnly, true) == kNoQueueFamily,
        "a present-only family is refused: the draw path needs graphics");
    Check(ChooseQueueFamily(presentOnly, false) == kNoQueueFamily,
        "and it is still refused when presentation is not required");

    const std::vector<QueueFamily> graphicsOnly = {{true, false, 1}};
    Check(ChooseQueueFamily(graphicsOnly, true) == kNoQueueFamily,
        "a graphics-only family is refused when a surface must be presented to");
    Check(ChooseQueueFamily(graphicsOnly, false) == 0,
        "but it is fine headless, where there is nothing to present to");
}

// The interesting case, and the reason a shared selector is not just the old loop
// moved: the first graphics family is NOT always the right answer. Picking it
// because it draws would give a device that cannot present.
void TestPrefersTheFamilyThatDoesBoth()
{
    const std::vector<QueueFamily> families = {
        {/*graphics=*/true,  /*present=*/false, 1},   // draws, cannot present
        {/*graphics=*/false, /*present=*/true,  1},   // presents, cannot draw
        {/*graphics=*/true,  /*present=*/true,  1},   // both
    };
    Check(ChooseQueueFamily(families, true) == 2,
        "the family that does BOTH is chosen over the earlier graphics-only one --"
        " taking family 0 because it draws would give a device that cannot present");
    Check(ChooseQueueFamily(families, false) == 0,
        "headless, the first graphics family is right and no surface is consulted");
}

// A family advertising zero queues cannot be used however capable it claims to be.
void TestEmptyFamiliesAreSkipped()
{
    const std::vector<QueueFamily> families = {
        {true, true, 0},    // capable but has no queues
        {true, true, 2},
    };
    Check(ChooseQueueFamily(families, true) == 1,
        "a family with queueCount 0 is skipped even when its flags look right");
}

// Nothing qualifies. The selector must SAY so.
void TestNoCandidateIsReportedNotGuessed()
{
    Check(ChooseQueueFamily({}, true) == kNoQueueFamily,
        "an empty list yields none rather than family 0");
    Check(ChooseQueueFamily({}, false) == kNoQueueFamily,
        "and that does not depend on whether presentation is needed");

    const std::vector<QueueFamily> useless = {{false, false, 4}, {false, true, 4}};
    Check(ChooseQueueFamily(useless, true) == kNoQueueFamily,
        "a device with no graphics family at all yields none -- returning 0 here"
        " would produce a device that silently cannot draw, and the symptom would"
        " appear much later as an empty window");
}

// Lowest qualifying index wins, so the choice is deterministic across runs. A
// selector that picked arbitrarily would make a GPU-dependent bug reproduce on one
// machine and not another.
void TestChoiceIsDeterministic()
{
    const std::vector<QueueFamily> families = {
        {false, false, 1}, {true, true, 1}, {true, true, 1},
    };
    Check(ChooseQueueFamily(families, true) == 1,
        "the lowest qualifying family is chosen, so the choice is reproducible");
}

} // namespace

int main()
{
    TestSingleCapableFamily();
    TestHalfCapableFamiliesAreRefused();
    TestPrefersTheFamilyThatDoesBoth();
    TestEmptyFamiliesAreSkipped();
    TestNoCandidateIsReportedNotGuessed();
    TestChoiceIsDeterministic();

    if (g_failures == 0)
    {
        printf("all queue family tests passed\n");
        return 0;
    }
    printf("%d queue family test(s) FAILED\n", g_failures);
    return 1;
}
