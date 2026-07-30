#include "gpu_queue_family.h"

namespace gears
{

uint32_t ChooseQueueFamily(const std::vector<QueueFamily>& families,
                           bool needPresent)
{
    for (uint32_t i = 0; i < families.size(); ++i)
    {
        const QueueFamily& family = families[i];
        if (family.count == 0)
            continue;               // capable on paper, no queues to use
        if (!family.graphics)
            continue;               // the draw path needs graphics
        if (needPresent && !family.present)
            continue;               // and the window needs presentation
        return i;
    }
    return kNoQueueFamily;
}

} // namespace gears
