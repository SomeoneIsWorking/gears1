#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>

#include "render_retirement.h"

int main()
{
    gears::RenderRetirement retirement;
    std::vector<uint32_t> order;

    std::function<void()> noWork = [&] { order.push_back(0); };
    assert(!retirement.Defer(noWork));
    assert(static_cast<bool>(noWork));
    noWork();

    const uint64_t first = retirement.AcceptFrame();
    std::function<void()> firstDone = [&] { order.push_back(1); };
    assert(retirement.Defer(firstDone));
    const uint64_t second = retirement.AcceptFrame();
    std::function<void()> secondDone = [&] { order.push_back(2); };
    assert(retirement.Defer(secondDone));

    auto batch = retirement.Finish(first);
    assert(!batch.generationComplete);
    assert(batch.completions.size() == 1);
    batch.completions.front()();
    batch = retirement.Finish(first);
    assert(batch.generationComplete);
    assert(batch.completions.empty());

    // Finishing the older render must not publish the completion attached to
    // the newer pending frame.
    assert((order == std::vector<uint32_t>{0, 1}));
    batch = retirement.Finish(second);
    assert(!batch.generationComplete);
    assert(batch.completions.size() == 1);
    batch.completions.front()();
    batch = retirement.Finish(second);
    assert(batch.generationComplete);
    assert((order == std::vector<uint32_t>{0, 1, 2}));

    std::function<void()> retired = [&] { order.push_back(3); };
    assert(!retirement.Defer(retired));
    assert(static_cast<bool>(retired));
    retired();
    assert((order == std::vector<uint32_t>{0, 1, 2, 3}));
}
