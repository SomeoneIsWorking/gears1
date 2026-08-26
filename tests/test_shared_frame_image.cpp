#include "gpu_shared_device.h"

#include <cstdint>
#include <cstdlib>
#include <memory>

namespace
{

void Check(bool condition)
{
    if (!condition)
        std::abort();
}

VkImage Image(size_t value)
{
    static uint8_t tokens[4]{};
    return reinterpret_cast<VkImage>(&tokens[value]);
}

gears::SharedFrameImage Frame(VkImage image, uint64_t sequence, const std::shared_ptr<void> &owner)
{
    gears::SharedFrameImage frame;
    frame.image = image;
    frame.width = 1280;
    frame.height = 720;
    frame.sequence = sequence;
    frame.lease = gears::SharedFrameImage::Lease(owner);
    return frame;
}

} // namespace

int main()
{
    gears::ClearSharedFrameImage();

    gears::SharedFrameImage invalid = Frame(Image(1), 1, {});
    Check(!gears::PublishSharedFrameImage(invalid));

    auto firstOwner = std::make_shared<uint32_t>(1);
    std::weak_ptr<void> firstLifetime = firstOwner;
    gears::SharedFrameImage first = Frame(Image(1), 1, firstOwner);
    Check(gears::PublishSharedFrameImage(first));
    firstOwner.reset();
    first.lease.Reset();
    Check(!firstLifetime.expired());

    gears::SharedFrameImage consumer;
    Check(gears::AcquireSharedFrameImage(consumer));
    Check(consumer.sequence == 1);

    auto secondOwner = std::make_shared<uint32_t>(2);
    gears::SharedFrameImage second = Frame(Image(2), 2, secondOwner);
    Check(gears::PublishSharedFrameImage(second));
    Check(!firstLifetime.expired());

    consumer.lease.Reset();
    Check(firstLifetime.expired());

    auto rejectedOwner = std::make_shared<uint32_t>(3);
    gears::SharedFrameImage duplicate = Frame(Image(3), 2, rejectedOwner);
    Check(!gears::PublishSharedFrameImage(duplicate));

    gears::SharedFrameImage latest;
    Check(gears::AcquireSharedFrameImage(latest));
    Check(latest.image == Image(2));
    Check(latest.sequence == 2);

    std::weak_ptr<void> secondLifetime = secondOwner;
    secondOwner.reset();
    second.lease.Reset();
    latest.lease.Reset();
    Check(!secondLifetime.expired());
    gears::ClearSharedFrameImage();
    Check(secondLifetime.expired());

    gears::SharedFrameImage empty;
    Check(!gears::AcquireSharedFrameImage(empty));
    return 0;
}
