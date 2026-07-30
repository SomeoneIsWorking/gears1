// Tests for the shared-VkInstance extension bookkeeping.
//
// Two devices in this runtime cost a full readback and staging upload of every
// rendered frame, because the drawn image and the swapchain live on different
// devices. Unifying them starts with the instance, since a surface created from one
// instance cannot be used with a device from another.
//
// The bookkeeping matters because the two callers need different extensions -- SDL's
// surface extensions for the present path, debug utils for the draw path when
// validation is on -- and whichever initialises first creates the instance. The
// failure this guards is the quiet one: handing a later caller an instance that
// lacks what it asked for, so the error surfaces at SDL_Vulkan_CreateSurface with a
// message that names nothing.

#include <cstdio>
#include <string>
#include <vector>

#include "gpu_instance.h"

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

using gears::InstanceExtensionsSatisfy;
using gears::MergeInstanceExtensions;

void TestSatisfiedWhenEverythingIsPresent()
{
    std::vector<std::string> missing;
    Check(InstanceExtensionsSatisfy({"VK_KHR_surface", "VK_KHR_xlib_surface"},
                                    {"VK_KHR_surface"}, missing),
        "a subset request is satisfied");
    Check(missing.empty(), "and nothing is reported missing");

    Check(InstanceExtensionsSatisfy({"a", "b"}, {}, missing),
        "an empty request is trivially satisfied");
}

// The case that matters: the instance was created by the headless draw path with
// only debug utils, and the present path then needs surface extensions.
void TestMissingExtensionsAreNamed()
{
    std::vector<std::string> missing;
    const bool ok = InstanceExtensionsSatisfy(
        {"VK_EXT_debug_utils"},
        {"VK_KHR_surface", "VK_KHR_xlib_surface"}, missing);
    Check(!ok, "an instance created without the surface extensions does NOT satisfy"
               " the present path");
    Check(missing.size() == 2,
        "and BOTH missing names are reported, not just the first -- the caller has"
        " to be able to say what it needed");
    Check(!missing.empty() && missing[0] == "VK_KHR_surface",
        "with the names themselves, so the log points at the cause rather than at"
        " a surface-creation failure one layer removed from it");
}

void TestMergeKeepsOrderAndDropsDuplicates()
{
    const auto merged = MergeInstanceExtensions(
        {"VK_KHR_surface", "VK_KHR_xlib_surface"},
        {"VK_KHR_surface", "VK_EXT_debug_utils"});
    Check(merged.size() == 3,
        "the duplicate is dropped -- Vulkan rejects a repeated extension name, and"
        " SDL's list overlaps ours");
    Check(merged[0] == "VK_KHR_surface" && merged[1] == "VK_KHR_xlib_surface" &&
              merged[2] == "VK_EXT_debug_utils",
        "and the order is first-list-then-new, so the result is deterministic");
}

void TestMergeWithEmptyLists()
{
    Check(MergeInstanceExtensions({}, {"a"}).size() == 1,
        "merging into an empty list yields the second");
    Check(MergeInstanceExtensions({"a"}, {}).size() == 1,
        "and merging nothing in leaves the first alone");
    Check(MergeInstanceExtensions({}, {}).empty(),
        "two empty lists merge to empty rather than to something");
}

// A repeated name WITHIN one list must also collapse, or the merged set can still
// contain a duplicate and instance creation fails with VK_ERROR_EXTENSION_NOT_PRESENT
// -- an error that reads as "the driver lacks it" rather than "we asked twice".
void TestDuplicatesWithinTheSecondListCollapse()
{
    const auto merged = MergeInstanceExtensions({"a"}, {"b", "b", "a"});
    Check(merged.size() == 2, "duplicates inside the incoming list collapse too");
}

} // namespace

int main()
{
    TestSatisfiedWhenEverythingIsPresent();
    TestMissingExtensionsAreNamed();
    TestMergeKeepsOrderAndDropsDuplicates();
    TestMergeWithEmptyLists();
    TestDuplicatesWithinTheSecondListCollapse();

    if (g_failures == 0)
    {
        printf("all gpu instance tests passed\n");
        return 0;
    }
    printf("%d gpu instance test(s) FAILED\n", g_failures);
    return 1;
}
