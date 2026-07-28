#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

#include "guest_memory.h"
#include "xma_decode.h"

namespace gears
{

// The XMA context protocol: the piece between "the codec opens" and "PCM in
// the title's output ring".
//
// This is a port of Xenia's current decoder (extern/xenia/src/xenia/apu/
// xma_context_new.cc), because the protocol is where the hard-won knowledge
// lives, not the codec: frames span packets, a frame's own 15-bit length
// header can straddle a packet boundary, packets with skip byte 0xFF carry
// only continuation data, and the consumer paces itself per 256-byte output
// block. Every one of those was learned by Xenia across a library of titles;
// reimplementing them from a summary is how they get lost. Where this file
// deviates from the reference it says so at the point of deviation.

// The layout of the 64-byte context as the hardware defines it. Stored in
// guest memory as sixteen big-endian words; loaded/stored with a whole-struct
// swap so host code can use the bitfields directly (same trick as Xenia's
// XMA_CONTEXT_DATA, xma_context.h:49).
struct XmaContextData
{
    // DWORD 0
    uint32_t inputBuffer0PacketCount : 12; // 2 KiB packets; these form a block
    uint32_t loopCount : 8;
    uint32_t inputBuffer0Valid : 1;
    uint32_t inputBuffer1Valid : 1;
    uint32_t outputBufferBlockCount : 5;   // 256-byte blocks
    uint32_t outputBufferWriteOffset : 5;  // in blocks; the decoder owns this

    // DWORD 1
    uint32_t inputBuffer1PacketCount : 12;
    uint32_t loopSubframeStart : 2;
    uint32_t loopSubframeEnd : 3;
    uint32_t loopSubframeSkip : 3;
    uint32_t subframeDecodeCount : 4;
    uint32_t outputBufferPadding : 3;
    uint32_t sampleRate : 2;               // enum, not Hz -- see kIdToSampleRate
    uint32_t isStereo : 1;
    uint32_t unkDword1 : 1;
    uint32_t outputBufferValid : 1;

    // DWORD 2
    uint32_t inputBufferReadOffset : 26;   // in BITS across the whole buffer
    uint32_t errorStatus : 5;
    uint32_t errorSet : 1;

    // DWORD 3
    uint32_t loopStart : 26;               // frame offset in bits
    uint32_t parserErrorStatus : 5;
    uint32_t parserErrorSet : 1;

    // DWORD 4
    uint32_t loopEnd : 26;                 // frame offset in bits
    uint32_t packetMetadata : 5;
    uint32_t currentBuffer : 1;

    // DWORDs 5-8, plain physical addresses
    uint32_t inputBuffer0Ptr;
    uint32_t inputBuffer1Ptr;
    uint32_t outputBufferPtr;
    uint32_t workBufferPtr;                // overlap-add scratch; unused here,
                                           // as in every Xenia variant

    // DWORD 9
    uint32_t outputBufferReadOffset : 5;   // in blocks; the title owns this
    uint32_t : 25;
    uint32_t stopWhenDone : 1;
    uint32_t interruptWhenDone : 1;        // not implemented -- see Work()

    // DWORDs 10-15
    uint32_t reserved[6];

    explicit XmaContextData(const void* guest);
    void Store(void* guest) const;

    bool IsInputBufferValid(uint32_t which) const
    {
        return which == 0 ? inputBuffer0Valid != 0 : inputBuffer1Valid != 0;
    }
    bool IsAnyInputBufferValid() const
    {
        return inputBuffer0Valid || inputBuffer1Valid;
    }
    bool IsCurrentInputBufferValid() const
    {
        return IsInputBufferValid(currentBuffer);
    }
    uint32_t GetInputBufferAddress(uint32_t which) const
    {
        return which == 0 ? inputBuffer0Ptr : inputBuffer1Ptr;
    }
    uint32_t GetCurrentInputBufferAddress() const
    {
        return GetInputBufferAddress(currentBuffer);
    }
    uint32_t GetInputBufferPacketCount(uint32_t which) const
    {
        return which == 0 ? inputBuffer0PacketCount : inputBuffer1PacketCount;
    }
    uint32_t GetCurrentInputBufferPacketCount() const
    {
        return GetInputBufferPacketCount(currentBuffer);
    }
    // A context the title only drains: both packet counts zero.
    bool IsConsumeOnly() const
    {
        return (inputBuffer0PacketCount | inputBuffer1PacketCount) == 0;
    }
};
static_assert(sizeof(XmaContextData) == 64, "the hardware context is 64 bytes");

// The stream geometry. All of it is the hardware's, none of it is tunable.
namespace xmaconst
{
constexpr uint32_t kBytesPerPacket = 2048;
constexpr uint32_t kBytesPerPacketHeader = 4;
constexpr uint32_t kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader;
constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
constexpr uint32_t kBitsPerPacketHeader = 32;
constexpr uint32_t kBitsPerFrameHeader = 15;
constexpr uint32_t kSamplesPerFrame = 512;
constexpr uint32_t kBytesPerSample = 2;
constexpr uint32_t kBytesPerFrameChannel = kSamplesPerFrame * kBytesPerSample;
constexpr uint32_t kOutputBytesPerBlock = 256;
constexpr uint32_t kMaxFrameLength = 0x7FFF;
constexpr uint32_t kMaxFrameSizeInBits = 0x4000 - kBitsPerPacketHeader;
constexpr uint32_t kIdToSampleRate[4] = {24000, 32000, 44100, 48000};
} // namespace xmaconst

// One hardware context's decoder side: everything the console's XMA block did
// for one slot of the array. Work() is synchronous -- when it returns, the
// guest context has been updated with everything the output ring had room
// for. That mirrors this Xenia's own kick handling (xma_decoder.cc waits for
// the worker before returning from the kick write) and means the title's
// kick/lock cycle needs no cross-thread choreography at all.
class XmaHwContext
{
public:
    explicit XmaHwContext(uint32_t index);
    ~XmaHwContext();

    // The kick: decode as much as the output ring can take, update the guest
    // context. Reports (once) the first frame actually decoded, because that
    // is the moment this subsystem stops being scaffolding.
    void Work(GuestMemory& memory, uint32_t guestPointer);

    // The clear register: reset the context the way the hardware does.
    void Clear(GuestMemory& memory, uint32_t guestPointer);

    // XMAReleaseContext: zero the slot; the owner destroys this object.
    void Release(GuestMemory& memory, uint32_t guestPointer);

private:
    struct PacketInfo
    {
        uint8_t frameCount = 0;
        uint8_t currentFrame = 0;
        uint32_t currentFrameSize = 0;
        bool IsLastFrameInPacket() const
        {
            return frameCount == 0 || currentFrame == frameCount - 1;
        }
    };
    struct PacketHandle
    {
        uint32_t bufferIndex = 0;
        uint32_t packetIndex = 0;
        bool valid = false;
    };

    void Decode(GuestMemory& memory, XmaContextData* data);
    void Consume(class RingWriter* ring, const XmaContextData* data);
    void UpdateLoopStatus(XmaContextData* data);
    bool PrepareDecoder(uint32_t sampleRateId, bool stereo);
    void ClearLocked(XmaContextData* data);

    static void SwapInputBuffer(XmaContextData* data);
    static int16_t GetPacketNumber(size_t bytes, size_t bitOffset);
    PacketInfo GetPacketInfo(uint8_t* packet, uint32_t frameOffset);
    PacketHandle GetPacketHandle(XmaContextData* data, uint32_t bufferIndex,
        uint32_t packetIndex, uint32_t currentPacketCount);
    const uint8_t* GetNextPacket(GuestMemory& memory, XmaContextData* data,
        uint32_t nextPacketIndex, uint32_t currentPacketCount);
    uint32_t GetNextPacketReadOffset(uint8_t* buffer, uint32_t nextPacketIndex,
        uint32_t packetCount);
    uint32_t GetNextPacketReadOffset(GuestMemory& memory, XmaContextData* data,
        uint32_t nextPacketIndex, uint32_t currentPacketCount);
    void StoreContextMerged(const XmaContextData& data,
        const XmaContextData& initial, uint8_t* guest);

    void TapWrite(const uint8_t* bytes, size_t count);

    const uint32_t index_;
    std::mutex mutex_;
    XmaFrameDecoder decoder_;

    // Two packets' worth of payload, so a frame that spans a packet boundary
    // can be reassembled contiguously before the codec sees it.
    std::array<uint8_t, xmaconst::kBytesPerPacketData * 2> inputBuffer_{};
    // One frame for the codec: byte 0 carries the bit-padding description the
    // xmaframes decoder requires, then up to 4096 bytes of frame.
    std::array<uint8_t, 1 + 4096> xmaFrame_{};
    // One decoded frame as the ring wants it: interleaved BIG-ENDIAN int16.
    std::array<uint8_t, xmaconst::kBytesPerFrameChannel * 2> rawFrame_{};
    std::vector<float> floatSamples_;

    int32_t remainingSubframeBlocks_ = 0;
    uint8_t currentFrameRemainingSubframes_ = 0;
    uint8_t loopFrameOutputLimit_ = 0;
    bool loopStartSkipPending_ = false;

    uint64_t framesDecoded_ = 0;
    uint64_t blocksWritten_ = 0;
    std::FILE* tap_ = nullptr;
    bool tapChecked_ = false;
};

} // namespace gears
