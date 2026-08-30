#include "bugcheck_policy.h"

#include <lucent/log.h>

namespace gears
{
namespace
{

const GuestBugCheckPolicy *ActivePolicy()
{
    static const GuestBugCheckPolicy *policy = []
    {
        const GuestBugCheckPolicy &linked = LinkedGuestBugCheckPolicy();
        if (!IsValidGuestBugCheckPolicy(linked))
        {
            lucent::error("kernel", "linked title supplied an invalid bug-check policy");
            return static_cast<const GuestBugCheckPolicy *>(nullptr);
        }
        return &linked;
    }();
    return policy;
}

} // namespace

std::string_view CurrentGuestBugCheckDescription(std::uint32_t code, std::uint32_t returnAddress)
{
    const GuestBugCheckPolicy *policy = ActivePolicy();
    return policy == nullptr ? std::string_view{}
                             : FindGuestBugCheckDescription(*policy, code, returnAddress);
}

} // namespace gears
