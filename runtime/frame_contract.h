#pragma once

#include <cstdint>
#include <mutex>

namespace gears
{

// Identity assigned at the guest-present boundary and carried unchanged through
// rendering, publication, and presentation. Zero is reserved for "no frame".
struct FrameId
{
    uint64_t value = 0;

    constexpr explicit operator bool() const { return value != 0; }
    constexpr auto operator<=>(const FrameId &) const = default;
};

enum class FrameTransition : uint8_t
{
    kAdvanced,
    kRepeated,
    kRejectedInvalid,
    kRejectedDuplicate,
    kRejectedRegression,
    kRejectedUnpublished,
    kRejectedStale,
};

struct FrameContractSnapshot
{
    FrameId published;
    FrameId presented;
};

// Owns the cross-thread identity contract between the renderer and presenter.
// Publications may skip dropped guest frames, but may never duplicate or move
// backward. Presentation may repeat the latest published image, but may never
// show an older or unpublished identity.
class FrameContract
{
  public:
    FrameTransition Publish(FrameId frame);
    FrameTransition Present(FrameId frame);
    FrameContractSnapshot Snapshot() const;

  private:
    mutable std::mutex mutex_;
    FrameId published_;
    FrameId presented_;
};

} // namespace gears
