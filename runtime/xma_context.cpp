#include "xma_context.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

#include <byteswap.h>

#include <lucent/config.h>
#include <lucent/log.h>

// A port of Xenia's xma_context_new.cc (the decoder its own selector defaults
// to) onto this runtime: GuestMemory instead of xe::Memory, XmaFrameDecoder
// instead of a raw AVCodecContext, lucent instead of XELOG, and synchronous
// operation instead of a worker thread. The protocol logic itself is kept
// line-for-line where possible; reference line numbers in comments are into
// extern/xenia/src/xenia/apu/xma_context_new.cc.

namespace gears
{

using namespace xmaconst;

namespace
{

// ---------------------------------------------------------------------------
// Bit reading, ported from xe::BitStream (extern/xenia/src/xenia/base/
// bit_stream.cc). XMA offsets are bit offsets and frames are bit-packed, so
// byte-level reading cannot express this protocol.
class BitReader
{
public:
    BitReader(const uint8_t* buffer, size_t sizeInBits)
        : buffer_(buffer), sizeBits_(sizeInBits) {}

    size_t OffsetBits() const { return offsetBits_; }
    size_t BitsRemaining() const { return sizeBits_ - offsetBits_; }

    void SetOffset(size_t offsetBits)
    {
        offsetBits_ = std::min(offsetBits, sizeBits_);
    }
    void Advance(size_t bits) { SetOffset(offsetBits_ + bits); }

    // Up to 57 bits: an unaligned read can only span 8 bytes. The buffer must
    // be at least 8 bytes long; every buffer this file reads is thousands.
    uint64_t Peek(size_t bits) const
    {
        const size_t offsetBytes =
            std::min(offsetBits_ >> 3, (sizeBits_ - 64) >> 3);
        const size_t relOffsetBits = offsetBits_ - (offsetBytes << 3);
        uint64_t value;
        std::memcpy(&value, buffer_ + offsetBytes, sizeof(value));
        value = ByteSwap(value); // the stream is big-endian
        value >>= 64 - (relOffsetBits + bits);
        return value & ((1ULL << bits) - 1);
    }

    uint64_t Read(size_t bits)
    {
        const uint64_t value = Peek(bits);
        Advance(bits);
        return value;
    }

    // Copies `bits` into dest, preserving the source's intra-byte alignment
    // (dest bit 0 of the copy lands at the same bit-in-byte position). Returns
    // that position -- the codec needs it as its padding_start. Port of
    // xe::BitStream::Copy.
    size_t Copy(uint8_t* dest, size_t bits)
    {
        const size_t relOffsetBits = offsetBits_ & 7;
        size_t bitsLeft = bits;
        size_t outBytes = 0;

        if (relOffsetBits)
        {
            const uint64_t head = Peek(8 - relOffsetBits);
            const uint8_t clearMask =
                uint8_t(~((uint8_t(1) << relOffsetBits) - 1));
            dest[outBytes] &= clearMask;
            dest[outBytes] |= uint8_t(head);
            bitsLeft -= 8 - relOffsetBits;
            Advance(8 - relOffsetBits);
            ++outBytes;
        }
        if (bitsLeft >= 8)
        {
            std::memcpy(dest + outBytes, buffer_ + (offsetBits_ >> 3),
                bitsLeft / 8);
            outBytes += bitsLeft / 8;
            Advance((bitsLeft / 8) * 8);
            bitsLeft -= (bitsLeft / 8) * 8;
        }
        if (bitsLeft)
        {
            const uint64_t tail = Peek(bitsLeft) << (8 - bitsLeft);
            const uint8_t clearMask = uint8_t((uint8_t(1) << bitsLeft) - 1);
            dest[outBytes] &= clearMask;
            dest[outBytes] |= uint8_t(tail);
            Advance(bitsLeft);
        }
        return relOffsetBits;
    }

private:
    const uint8_t* buffer_;
    size_t offsetBits_ = 0;
    size_t sizeBits_;
};

// Packet-header fields, ported from xma_helpers.h. The first-frame offset is
// biased by the 32-bit header; a value beyond kMaxFrameSizeInBits means no
// frame begins in this packet.
uint8_t PacketFrameCount(const uint8_t* packet) { return packet[0] >> 2; }
uint8_t PacketMetadata(const uint8_t* packet) { return packet[2] & 0x7; }
bool IsPacketXma2(const uint8_t* packet) { return PacketMetadata(packet) == 1; }
uint8_t PacketSkipCount(const uint8_t* packet) { return packet[3]; }
uint32_t PacketFrameOffset(const uint8_t* packet)
{
    const uint32_t value = uint32_t(((packet[0] & 0x3) << 13) |
        (packet[1] << 5) | (packet[2] >> 3));
    return value + 32;
}

} // namespace

// ---------------------------------------------------------------------------
// The output ring, semantics ported from xe::RingBuffer as the reference uses
// it: offsets in bytes, read==write meaning "empty" as a state but "capacity
// free" for write_count -- the protocol resolves the ambiguity itself by
// counting blocks.
class RingWriter
{
public:
    RingWriter(uint8_t* buffer, uint32_t capacity)
        : buffer_(buffer), capacity_(capacity) {}

    void SetReadOffset(uint32_t offset) { readOffset_ = offset % capacity_; }
    void SetWriteOffset(uint32_t offset) { writeOffset_ = offset % capacity_; }
    uint32_t WriteOffset() const { return writeOffset_; }
    bool Empty() const { return readOffset_ == writeOffset_; }

    uint32_t WriteCount() const
    {
        if (readOffset_ == writeOffset_)
            return capacity_;
        if (writeOffset_ < readOffset_)
            return readOffset_ - writeOffset_;
        return (capacity_ - writeOffset_) + readOffset_;
    }

    void Write(const uint8_t* source, uint32_t count)
    {
        const uint32_t untilEnd = capacity_ - writeOffset_;
        const uint32_t first = std::min(count, untilEnd);
        std::memcpy(buffer_ + writeOffset_, source, first);
        std::memcpy(buffer_, source + first, count - first);
        writeOffset_ = (writeOffset_ + count) % capacity_;
    }

private:
    uint8_t* buffer_;
    uint32_t capacity_;
    uint32_t readOffset_ = 0;
    uint32_t writeOffset_ = 0;
};

// ---------------------------------------------------------------------------
// Context data <-> guest memory: sixteen big-endian words, swapped whole.

XmaContextData::XmaContextData(const void* guest)
{
    const uint32_t* source = static_cast<const uint32_t*>(guest);
    uint32_t* destination = reinterpret_cast<uint32_t*>(this);
    for (size_t i = 0; i < sizeof(*this) / 4; ++i)
        destination[i] = ByteSwap(source[i]);
}

void XmaContextData::Store(void* guest) const
{
    const uint32_t* source = reinterpret_cast<const uint32_t*>(this);
    uint32_t* destination = static_cast<uint32_t*>(guest);
    for (size_t i = 0; i < sizeof(*this) / 4; ++i)
        destination[i] = ByteSwap(source[i]);
}

// ---------------------------------------------------------------------------

XmaHwContext::XmaHwContext(uint32_t index) : index_(index) {}

XmaHwContext::~XmaHwContext()
{
    if (tap_)
        std::fclose(tap_);
}

// GEARS_XMA_TAP=<dir> appends every byte committed to the output ring to
// <dir>/ctx<index>.pcm, exactly as written (interleaved big-endian int16).
// This is the runtime's side of the golden-reference comparison: the offline
// decode of the same dump is the other side, and the two must match.
void XmaHwContext::TapWrite(const uint8_t* bytes, size_t count)
{
    if (!tapChecked_)
    {
        tapChecked_ = true;
        const std::string& dir = lucent::config::text("XMA_TAP");
        if (!dir.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::string path =
                dir + "/ctx" + std::to_string(index_) + ".pcm";
            tap_ = std::fopen(path.c_str(), "wb");
            if (!tap_)
                lucent::warn("xma", "context {}: tap {} did not open", index_,
                    path);
        }
    }
    if (tap_)
        std::fwrite(bytes, 1, count, tap_);
}

// Reference: ClearLocked, xma_context_new.cc:250. The rest value of the input
// read offset is 32 -- the bit width of the packet header -- not 0; resetting
// to 0 was a bug the older Xenia variants carried.
void XmaHwContext::ClearLocked(XmaContextData* data)
{
    data->inputBuffer0Valid = 0;
    data->inputBuffer1Valid = 0;
    data->outputBufferValid = 0;
    data->inputBufferReadOffset = kBitsPerPacketHeader;
    data->outputBufferReadOffset = 0;
    data->outputBufferWriteOffset = 0;
    currentFrameRemainingSubframes_ = 0;
    loopFrameOutputLimit_ = 0;
    loopStartSkipPending_ = false;
}

void XmaHwContext::Clear(GuestMemory& memory, uint32_t guestPointer)
{
    std::lock_guard<std::mutex> guard(mutex_);
    lucent::debug("xma", "context {}: clear", index_);
    uint8_t* guest = memory.Translate<uint8_t>(guestPointer);
    XmaContextData data(guest);
    ClearLocked(&data);
    data.Store(guest);
}

void XmaHwContext::Release(GuestMemory& memory, uint32_t guestPointer)
{
    std::lock_guard<std::mutex> guard(mutex_);
    std::memset(memory.Translate<uint8_t>(guestPointer), 0,
        sizeof(XmaContextData));
}

// Reference: SwapInputBuffer, xma_context_new.cc:282.
void XmaHwContext::SwapInputBuffer(XmaContextData* data)
{
    if (data->currentBuffer == 0)
        data->inputBuffer0Valid = 0;
    else
        data->inputBuffer1Valid = 0;
    data->currentBuffer ^= 1;
    data->inputBufferReadOffset = kBitsPerPacketHeader;
}

// Reference: GetPacketNumber, xma_context_new.cc:876.
int16_t XmaHwContext::GetPacketNumber(size_t bytes, size_t bitOffset)
{
    if (bitOffset < kBitsPerPacketHeader)
        return -1;
    if (bitOffset >= (bytes << 3))
        return -1;
    return int16_t((bitOffset >> 3) / kBytesPerPacket);
}

// Reference: GetPacketInfo, xma_context_new.cc:812. Walks the frame chain in
// one packet: 15-bit length headers, a trailing continuation bit after each
// frame. A frame whose header is split across the packet boundary leaves
// currentFrameSize at 0, which Decode resolves by combining packets. The walk
// detects that frame two ways: the XMA2 packet header's frame count (the
// reference's method), or -- for the XMA1 packets this title actually ships,
// which have no such count -- the last walked frame's continuation bit, which
// the reference does not use and therefore loses one frame per occurrence.
XmaHwContext::PacketInfo XmaHwContext::GetPacketInfo(uint8_t* packet,
    uint32_t frameOffset)
{
    PacketInfo info;

    const uint32_t firstFrameOffset = PacketFrameOffset(packet);
    BitReader stream(packet, kBitsPerPacket);
    stream.SetOffset(firstFrameOffset);

    if (frameOffset < firstFrameOffset)
    {
        // Tail of a frame split from the previous packet.
        info.currentFrame = 0;
        info.currentFrameSize = firstFrameOffset - frameOffset;
    }

    bool splitHeaderPending = false;
    while (true)
    {
        if (stream.BitsRemaining() < kBitsPerFrameHeader)
        {
            // The previous frame's continuation bit said another frame
            // follows, and PART of it is in what is left of this packet: its
            // 15-bit header is split across the boundary. Zero bits left is
            // different -- then the next frame begins cleanly in the next
            // packet (at its own first-frame offset) and counting it here
            // would both claim a frame this packet does not have and send
            // the reference's `% kBitsPerPacket` next-offset arithmetic
            // wrapping back to this packet's own start, decoding it forever.
            splitHeaderPending =
                info.frameCount > 0 && stream.BitsRemaining() > 0;
            break;
        }

        const uint64_t frameSize = stream.Peek(kBitsPerFrameHeader);
        if (frameSize == 0 || frameSize == kMaxFrameLength)
            break;

        if (stream.OffsetBits() == frameOffset)
        {
            info.currentFrame = info.frameCount;
            info.currentFrameSize = uint32_t(frameSize);
        }

        ++info.frameCount;

        if (frameSize > stream.BitsRemaining())
            break; // last frame, continues in the next packet

        stream.Advance(frameSize - 1);
        if (stream.Read(1) == 0)
            break; // no continuation bit: last frame of the packet
    }

    if (IsPacketXma2(packet))
    {
        const uint8_t headerFrameCount = PacketFrameCount(packet);
        if (headerFrameCount > info.frameCount)
        {
            // The chain walk could not peek a full 15-bit header before the
            // packet ended: the header itself is split across the boundary.
            // XMA2 advertises the packet's frame count, which resolves it.
            if (info.currentFrameSize == 0)
                info.currentFrame = info.frameCount;
            info.frameCount = headerFrameCount;
        }
        else if (headerFrameCount != info.frameCount)
        {
            lucent::warn("xma", "context {}: packet header says {} frames,"
                " the chain walk found {}", index_, headerFrameCount,
                info.frameCount);
        }
    }
    else if (splitHeaderPending)
    {
        // XMA1 packets carry a sequence number where XMA2 carries the frame
        // count, so there is no count to consult -- the reference declares
        // this case undetectable and silently loses the frame
        // (xma_context_new.cc:487-489). The continuation bit detects it for
        // both variants, and this title's streams ARE XMA1: without this,
        // one frame vanishes at every such boundary (measured: 4 frames in
        // one 142-second music stream, each an audible dropout and a lag
        // step against the golden reference).
        lucent::debug("xma", "context {}: split frame header after frame {}"
            " (XMA1, detected by continuation bit)", index_, info.frameCount);
        if (info.currentFrameSize == 0)
            info.currentFrame = info.frameCount;
        ++info.frameCount;
    }
    return info;
}

// Reference: GetPacketHandle, xma_context_new.cc:701. A packet index past the
// current buffer's end refers into the OTHER buffer -- that is how a frame
// spanning the last packet of buffer 0 and the first of buffer 1 is reached.
XmaHwContext::PacketHandle XmaHwContext::GetPacketHandle(XmaContextData* data,
    uint32_t bufferIndex, uint32_t packetIndex, uint32_t currentPacketCount)
{
    PacketHandle handle;
    const bool inNextBuffer = packetIndex >= currentPacketCount;
    if (inNextBuffer)
    {
        bufferIndex ^= 1;
        packetIndex -= currentPacketCount;
    }

    if (!data->IsInputBufferValid(bufferIndex))
        return handle;

    const uint32_t address = data->GetInputBufferAddress(bufferIndex);
    if (!address)
    {
        lucent::error("xma", "context {}: buffer {} marked valid but is a null"
            " pointer", index_, bufferIndex);
        return handle;
    }
    if (packetIndex >= data->GetInputBufferPacketCount(bufferIndex))
    {
        lucent::error("xma", "context {}: packet {} is past buffer {}'s {}"
            " packets", index_, packetIndex, bufferIndex,
            data->GetInputBufferPacketCount(bufferIndex));
        return handle;
    }

    handle.bufferIndex = bufferIndex;
    handle.packetIndex = packetIndex;
    handle.valid = true;
    return handle;
}

const uint8_t* XmaHwContext::GetNextPacket(GuestMemory& memory,
    XmaContextData* data, uint32_t nextPacketIndex, uint32_t currentPacketCount)
{
    const PacketHandle handle = GetPacketHandle(data, data->currentBuffer,
        nextPacketIndex, currentPacketCount);
    if (!handle.valid)
        return nullptr;
    return memory.Translate<uint8_t>(
        data->GetInputBufferAddress(handle.bufferIndex)) +
        handle.packetIndex * kBytesPerPacket;
}

// Reference: GetNextPacketReadOffset (buffer form), xma_context_new.cc:758.
// Scans forward for the first packet in which a frame actually begins.
uint32_t XmaHwContext::GetNextPacketReadOffset(uint8_t* buffer,
    uint32_t nextPacketIndex, uint32_t packetCount)
{
    while (nextPacketIndex < packetCount)
    {
        uint8_t* packet = buffer + nextPacketIndex * kBytesPerPacket;
        const uint32_t frameOffset = PacketFrameOffset(packet);
        if (frameOffset <= kMaxFrameSizeInBits)
            return nextPacketIndex * kBitsPerPacket + frameOffset;
        ++nextPacketIndex;
    }
    return kBitsPerPacketHeader;
}

uint32_t XmaHwContext::GetNextPacketReadOffset(GuestMemory& memory,
    XmaContextData* data, uint32_t nextPacketIndex, uint32_t currentPacketCount)
{
    const PacketHandle handle = GetPacketHandle(data, data->currentBuffer,
        nextPacketIndex, currentPacketCount);
    if (!handle.valid)
        return kBitsPerPacketHeader;
    return GetNextPacketReadOffset(
        memory.Translate<uint8_t>(
            data->GetInputBufferAddress(handle.bufferIndex)),
        handle.packetIndex, data->GetInputBufferPacketCount(handle.bufferIndex));
}

// Reference: UpdateLoopStatus, xma_context_new.cc:677. Loop offsets are frame
// bit offsets; a loop count of 255 means forever. Nothing observed from this
// title loops yet (both dumped contexts carry loopCount 0), so this path is
// ported but UNVERIFIED against real data -- it is the reference's logic, not
// proven behavior of this runtime.
void XmaHwContext::UpdateLoopStatus(XmaContextData* data)
{
    if (data->loopCount == 0)
        return;

    const uint32_t loopStart =
        std::max(kBitsPerPacketHeader, uint32_t(data->loopStart));
    const uint32_t loopEnd =
        std::max(kBitsPerPacketHeader, uint32_t(data->loopEnd));

    if (data->inputBufferReadOffset != loopEnd)
        return;

    lucent::debug("xma", "context {}: loop to {} ({} remaining)", index_,
        loopStart, uint32_t(data->loopCount));
    data->inputBufferReadOffset = loopStart;
    loopStartSkipPending_ = true;
    if (data->loopCount != 255)
        data->loopCount = data->loopCount - 1;
}

bool XmaHwContext::PrepareDecoder(uint32_t sampleRateId, bool stereo)
{
    const uint32_t sampleRate = kIdToSampleRate[std::min(sampleRateId, 3u)];
    const uint32_t channels = stereo ? 2u : 1u;
    if (decoder_.IsOpen() && decoder_.sampleRate() == sampleRate &&
        decoder_.channels() == channels)
        return true;
    return decoder_.Open(sampleRate, stereo);
}

// Reference: Decode, xma_context_new.cc:368. One call decodes AT MOST one
// frame into rawFrame_ and computes the next input read offset; Consume()
// dribbles the frame into the ring at the pace the context asks for.
void XmaHwContext::Decode(GuestMemory& memory, XmaContextData* data)
{
    if (!data->IsAnyInputBufferValid())
        return;

    // A previous frame is still being consumed; nothing to decode yet.
    if (currentFrameRemainingSubframes_ > 0)
        return;

    if (!data->IsCurrentInputBufferValid())
    {
        SwapInputBuffer(data);
        if (!data->IsCurrentInputBufferValid())
            return;
    }

    uint8_t* currentInputBuffer =
        memory.Translate<uint8_t>(data->GetCurrentInputBufferAddress());

    inputBuffer_.fill(0);

    // Detect the loop-end frame BEFORE UpdateLoopStatus rewrites the offset;
    // its output is truncated at loopSubframeEnd (reference :399).
    bool isLoopEndFrame = false;
    if (data->loopCount > 0)
    {
        const uint32_t loopEnd =
            std::max(kBitsPerPacketHeader, uint32_t(data->loopEnd));
        isLoopEndFrame = data->inputBufferReadOffset == loopEnd;
    }

    UpdateLoopStatus(data);

    // Some titles kick with a read offset still inside the packet header
    // (reference cites Dirt 2); clamp to the first valid bit position.
    if (data->inputBufferReadOffset < kBitsPerPacketHeader)
    {
        lucent::debug("xma", "context {}: read offset {} is inside the packet"
            " header, clamping to {}", index_, uint32_t(data->inputBufferReadOffset),
            kBitsPerPacketHeader);
        data->inputBufferReadOffset = kBitsPerPacketHeader;
    }

    const uint32_t currentInputSize =
        data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
    const uint32_t currentInputPacketCount = currentInputSize / kBytesPerPacket;

    const int16_t packetIndex =
        GetPacketNumber(currentInputSize, data->inputBufferReadOffset);
    if (packetIndex < 0)
    {
        lucent::error("xma", "context {}: read offset {} maps to no packet in"
            " a {}-byte buffer", index_, uint32_t(data->inputBufferReadOffset),
            currentInputSize);
        return;
    }

    uint8_t* packet = currentInputBuffer + packetIndex * kBytesPerPacket;
    const uint32_t packetFirstFrameOffset = PacketFrameOffset(packet);
    uint32_t relativeOffset = data->inputBufferReadOffset % kBitsPerPacket;

    // Before the first frame of the packet means inside the tail of a frame
    // whose beginning we never had; skip to the first whole frame.
    if (relativeOffset < packetFirstFrameOffset)
    {
        lucent::debug("xma", "context {}: skipping split-frame tail in packet"
            " {} ({} -> {})", index_, packetIndex, relativeOffset,
            packetFirstFrameOffset);
        data->inputBufferReadOffset =
            packetIndex * kBitsPerPacket + packetFirstFrameOffset;
        relativeOffset = packetFirstFrameOffset;
    }

    const uint8_t skipCount = PacketSkipCount(packet);

    // 0xFF: no frame begins in this packet at all -- it is pure continuation
    // data. Advance to the next packet (reference :466).
    if (skipCount == 0xFF)
    {
        const uint32_t skipToIndex = packetIndex + 1;
        const uint32_t nextOffset = GetNextPacketReadOffset(memory, data,
            skipToIndex, currentInputPacketCount);
        if (skipToIndex >= currentInputPacketCount ||
            nextOffset == kBitsPerPacketHeader)
            SwapInputBuffer(data);
        data->inputBufferReadOffset = nextOffset;
        return;
    }

    PacketInfo packetInfo = GetPacketInfo(packet, relativeOffset);
    const uint32_t nextPacketIndex = packetIndex + skipCount + 1;

    // The frame's own 15-bit length header is split across the packet
    // boundary: combine this packet with the next to read it (reference :490).
    if (packetInfo.currentFrameSize == 0)
    {
        const uint8_t* nextPacket =
            GetNextPacket(memory, data, nextPacketIndex, currentInputPacketCount);
        if (!nextPacket)
        {
            // Cannot resolve the header without the next buffer; consume this
            // one and move on, as the reference does.
            lucent::debug("xma", "context {}: split frame header at packet {}"
                " with no next buffer; swapping", index_, packetIndex);
            SwapInputBuffer(data);
            return;
        }
        std::memcpy(inputBuffer_.data(), packet + kBytesPerPacketHeader,
            kBytesPerPacketData);
        std::memcpy(inputBuffer_.data() + kBytesPerPacketData,
            nextPacket + kBytesPerPacketHeader, kBytesPerPacketData);

        BitReader combined(inputBuffer_.data(),
            (kBitsPerPacket - kBitsPerPacketHeader) * 2);
        combined.SetOffset(relativeOffset - kBitsPerPacketHeader);
        const uint64_t frameSize = combined.Peek(kBitsPerFrameHeader);
        if (frameSize == kMaxFrameLength)
        {
            lucent::warn("xma", "context {}: split header resolved to the"
                " end-of-stream marker", index_);
            data->errorStatus = 4;
            return;
        }
        packetInfo.currentFrameSize = uint32_t(frameSize);
    }

    BitReader stream(currentInputBuffer,
        size_t(packetIndex + 1) * kBitsPerPacket);
    stream.SetOffset(data->inputBufferReadOffset);

    const uint32_t bitsToCopy = std::min(uint32_t(stream.BitsRemaining()),
        packetInfo.currentFrameSize);
    if (bitsToCopy == 0)
    {
        lucent::warn("xma", "context {}: no bits to copy at offset {}", index_,
            uint32_t(data->inputBufferReadOffset));
        SwapInputBuffer(data);
        return;
    }

    if (packetInfo.IsLastFrameInPacket() &&
        stream.BitsRemaining() < packetInfo.currentFrameSize)
    {
        // The frame body continues in the next packet.
        const uint8_t* nextPacket =
            GetNextPacket(memory, data, nextPacketIndex, currentInputPacketCount);
        if (!nextPacket)
        {
            // The reference sets error 4 here too, with the note that the
            // real hardware error code is unknown.
            data->errorStatus = 4;
            return;
        }
        std::memcpy(inputBuffer_.data() + kBytesPerPacketData,
            nextPacket + kBytesPerPacketHeader, kBytesPerPacketData);
    }

    std::memcpy(inputBuffer_.data(), packet + kBytesPerPacketHeader,
        kBytesPerPacketData);

    BitReader frameStream(inputBuffer_.data(),
        (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    frameStream.SetOffset(relativeOffset - kBitsPerPacketHeader);

    xmaFrame_.fill(0);
    const uint32_t paddingStart = uint32_t(
        frameStream.Copy(xmaFrame_.data() + 1, packetInfo.currentFrameSize));

    // The xmaframes codec's input contract (extern/ffmpeg-xmaframes/
    // libavcodec/wmaprodec.c, xmaframes_decode_packet): byte 0 is
    // padding_start(3):padding_end(3):unused(2), then the frame at that bit
    // position, and the total size must be exact.
    const uint32_t frameBits = paddingStart + packetInfo.currentFrameSize;
    const uint32_t packetBytes = 1 + frameBits / 8 + (frameBits % 8 ? 1 : 0);
    const uint32_t paddingEnd = packetBytes * 8 - (8 + frameBits);
    xmaFrame_[0] = uint8_t(((paddingStart & 7) << 5) | ((paddingEnd & 7) << 2));

    if (!PrepareDecoder(data->sampleRate, data->isStereo != 0))
    {
        // DELIBERATE deviation from the reference, which advances the read
        // offset even with no codec behind it. Consuming input while
        // producing nothing is the exact "offsets advance, nothing decodes"
        // this project forbids; a context with no decoder stays frozen, which
        // is also the observable symptom that something is wrong.
        return;
    }

    {
        // One line per frame, channel-gated: the trace an independent frame
        // walk of the same dump is diffed against when samples go missing.
        lucent::debug("xma", "context {}: frame at {} ({} bits, packet {},"
            " {}/{})", index_, uint32_t(data->inputBufferReadOffset),
            packetInfo.currentFrameSize, packetIndex,
            uint32_t(packetInfo.currentFrame), uint32_t(packetInfo.frameCount));

        rawFrame_.fill(0);
        const int samples =
            decoder_.Decode(xmaFrame_.data(), packetBytes, floatSamples_);
        if (samples > 0)
        {
            const uint32_t channels = data->isStereo ? 2 : 1;
            if (uint32_t(samples) != kSamplesPerFrame)
                lucent::warn("xma", "context {}: frame decoded to {} samples,"
                    " not {}", index_, samples, kSamplesPerFrame);

            // The ring wants interleaved BIG-ENDIAN int16; the decoder hands
            // back interleaved host float. Saturate: FFmpeg output is not
            // limited to [-1, 1] (reference xma_context.cc:59).
            const size_t count = std::min(size_t(samples) * channels,
                rawFrame_.size() / kBytesPerSample);
            int16_t* out = reinterpret_cast<int16_t*>(rawFrame_.data());
            for (size_t i = 0; i < count; ++i)
            {
                const float scaled =
                    std::clamp(floatSamples_[i], -1.0f, 1.0f) * 32767.0f;
                out[i] = int16_t(ByteSwap(uint16_t(int16_t(scaled))));
            }

            currentFrameRemainingSubframes_ = uint8_t(4 << data->isStereo);
            ++framesDecoded_;
            if (framesDecoded_ == 1)
                lucent::info("xma", "context {}: first frame decoded ({} Hz,"
                    " {}, {} bits at offset {})", index_,
                    kIdToSampleRate[std::min(uint32_t(data->sampleRate), 3u)],
                    data->isStereo ? "stereo" : "mono",
                    packetInfo.currentFrameSize,
                    uint32_t(data->inputBufferReadOffset));

            // Loop-end truncation and loop-start subframe skip (reference
            // :592-615). Unverified against real data: this title loops
            // nothing yet.
            if (isLoopEndFrame)
                loopFrameOutputLimit_ =
                    uint8_t((data->loopSubframeEnd + 1) << data->isStereo);
            else
                loopFrameOutputLimit_ = 0;
            if (loopStartSkipPending_)
            {
                const uint8_t skip =
                    uint8_t(data->loopSubframeSkip << data->isStereo);
                if (skip < currentFrameRemainingSubframes_)
                    currentFrameRemainingSubframes_ =
                        uint8_t(currentFrameRemainingSubframes_ - skip);
                loopStartSkipPending_ = false;
            }
        }
        else
        {
            // A frame that decodes to nothing is 512 lost samples -- an
            // audible dropout and a lag shift against any reference. The
            // offset still advances, exactly as the reference behaves when
            // DecodePacket fails (the frame is consumed either way, or the
            // stream would stall on it forever), but it is a WARN, not a
            // debug line: silence about lost audio is how a 0.68 correlation
            // hides.
            lucent::warn("xma", "context {}: frame at offset {} ({} bits)"
                " produced no samples ({})", index_,
                uint32_t(data->inputBufferReadOffset),
                packetInfo.currentFrameSize, samples);
        }
    }

    // Where to go next (reference :618).
    if (!packetInfo.IsLastFrameInPacket())
    {
        const uint32_t nextFrameOffset =
            (data->inputBufferReadOffset + bitsToCopy) % kBitsPerPacket;
        data->inputBufferReadOffset =
            packetIndex * kBitsPerPacket + nextFrameOffset;
        return;
    }

    uint32_t nextInputOffset = GetNextPacketReadOffset(memory, data,
        nextPacketIndex, currentInputPacketCount);

    if (nextPacketIndex >= currentInputPacketCount ||
        nextInputOffset == kBitsPerPacketHeader)
        SwapInputBuffer(data);

    if (nextInputOffset == kBitsPerPacketHeader)
    {
        // Start of the next buffer: land on its first frame, or skip the
        // buffer entirely if no frame begins in its first packet.
        if (data->IsAnyInputBufferValid())
        {
            nextInputOffset = PacketFrameOffset(memory.Translate<uint8_t>(
                data->GetCurrentInputBufferAddress()));
            if (nextInputOffset > kMaxFrameSizeInBits)
            {
                lucent::debug("xma", "context {}: next buffer's first packet"
                    " has no frames (offset {})", index_, nextInputOffset);
                SwapInputBuffer(data);
                return;
            }
        }
    }
    data->inputBufferReadOffset = nextInputOffset;
}

// Reference: Consume, xma_context_new.cc:295. Writes at most
// subframeDecodeCount 256-byte blocks of the decoded frame per pass.
void XmaHwContext::Consume(RingWriter* ring, const XmaContextData* data)
{
    if (!currentFrameRemainingSubframes_)
        return;

    const uint8_t totalSubframes =
        uint8_t((kBytesPerFrameChannel / kOutputBytesPerBlock)
            << data->isStereo);

    // Loop-end truncation: discard the rest of the frame past the limit.
    if (loopFrameOutputLimit_ > 0)
    {
        const uint8_t consumed =
            uint8_t(totalSubframes - currentFrameRemainingSubframes_);
        if (consumed >= loopFrameOutputLimit_)
        {
            remainingSubframeBlocks_ -= data->outputBufferPadding;
            currentFrameRemainingSubframes_ = 0;
            loopFrameOutputLimit_ = 0;
            return;
        }
    }

    // subframeDecodeCount of 0 would make zero progress forever; the
    // reference treats it as 1.
    const uint8_t effectiveCount =
        uint8_t(std::max(1u, uint32_t(data->subframeDecodeCount)));
    int8_t subframesToWrite = int8_t(std::min(currentFrameRemainingSubframes_,
        effectiveCount));

    if (loopFrameOutputLimit_ > 0)
    {
        const uint8_t consumed =
            uint8_t(totalSubframes - currentFrameRemainingSubframes_);
        const int8_t untilLimit = int8_t(loopFrameOutputLimit_ - consumed);
        subframesToWrite = std::min(subframesToWrite, untilLimit);
    }

    const uint8_t frameReadOffset =
        uint8_t(totalSubframes - currentFrameRemainingSubframes_);
    const uint8_t* source =
        rawFrame_.data() + size_t(kOutputBytesPerBlock) * frameReadOffset;
    const uint32_t bytes = uint32_t(subframesToWrite) * kOutputBytesPerBlock;

    ring->Write(source, bytes);
    TapWrite(source, bytes);
    blocksWritten_ += uint32_t(subframesToWrite);

    // outputBufferPadding blocks of headroom are charged when the frame
    // completes, per the reference's reading of the field.
    const int8_t headroom =
        (currentFrameRemainingSubframes_ - subframesToWrite == 0)
            ? int8_t(data->outputBufferPadding) : int8_t(0);
    remainingSubframeBlocks_ -= subframesToWrite + headroom;
    currentFrameRemainingSubframes_ =
        uint8_t(currentFrameRemainingSubframes_ - subframesToWrite);
}

// Reference: StoreContextMerged, xma_context_new.cc:958. The title can write
// its half of the context between our load and our store; writing back the
// whole stale struct would clobber it. Merge only the fields the decoder
// owns into a FRESH snapshot.
void XmaHwContext::StoreContextMerged(const XmaContextData& data,
    const XmaContextData& initial, uint8_t* guest)
{
    XmaContextData fresh(guest);

    fresh.loopCount = data.loopCount;
    fresh.outputBufferWriteOffset = data.outputBufferWriteOffset;
    if (initial.inputBuffer0Valid && !data.inputBuffer0Valid)
        fresh.inputBuffer0Valid = 0;
    if (initial.inputBuffer1Valid && !data.inputBuffer1Valid)
        fresh.inputBuffer1Valid = 0;
    if (initial.outputBufferValid && !data.outputBufferValid)
        fresh.outputBufferValid = 0;
    fresh.inputBufferReadOffset = data.inputBufferReadOffset;
    fresh.errorStatus = data.errorStatus;
    fresh.currentBuffer = data.currentBuffer;
    fresh.outputBufferReadOffset = data.outputBufferReadOffset;

    fresh.Store(guest);
}

// Reference: Work, xma_context_new.cc:113. Synchronous: the worker thread and
// enable/disable flags of the reference exist to bridge its kick register to
// a separate thread; a kick here IS the call.
void XmaHwContext::Work(GuestMemory& memory, uint32_t guestPointer)
{
    std::lock_guard<std::mutex> guard(mutex_);

    uint8_t* guest = memory.Translate<uint8_t>(guestPointer);
    XmaContextData data(guest);
    const XmaContextData initial = data;

    if (!data.outputBufferValid)
        return;

    if (!data.outputBufferBlockCount)
    {
        // The reference checks this inside Decode, but a zero-capacity ring
        // cannot even be constructed (offsets are taken modulo the capacity),
        // so the check happens before the ring does.
        lucent::warn("xma", "context {}: kicked with a zero-block output"
            " buffer", index_);
        return;
    }

    if (data.interruptWhenDone && framesDecoded_ == 0)
        lucent::warn("xma", "context {}: asks for interrupt-when-done, which"
            " is not implemented (no Xenia variant implements it either)",
            index_);

    const uint32_t capacity = data.outputBufferBlockCount * kOutputBytesPerBlock;
    RingWriter ring(memory.Translate<uint8_t>(data.outputBufferPtr), capacity);
    ring.SetReadOffset(data.outputBufferReadOffset * kOutputBytesPerBlock);
    ring.SetWriteOffset(data.outputBufferWriteOffset * kOutputBytesPerBlock);
    remainingSubframeBlocks_ =
        int32_t(ring.WriteCount() / kOutputBytesPerBlock);

    if (data.IsConsumeOnly())
    {
        // Nothing queued to decode; only drain what a previous frame left.
        if (currentFrameRemainingSubframes_ == 0)
            return;
        Consume(&ring, &data);
        data.outputBufferWriteOffset =
            ring.WriteOffset() / kOutputBytesPerBlock;
        if (ring.Empty())
            data.outputBufferValid = 0;
        StoreContextMerged(data, initial, guest);
        return;
    }

    const uint32_t effectiveCount =
        std::max(1u, uint32_t(data.subframeDecodeCount));
    const int32_t minimumBlocks =
        int32_t(effectiveCount) + int32_t(data.outputBufferPadding);

    if (minimumBlocks > remainingSubframeBlocks_)
    {
        // No room for even one pass; the title has not consumed yet.
        StoreContextMerged(data, initial, guest);
        return;
    }

    while (remainingSubframeBlocks_ >= minimumBlocks)
    {
        const uint32_t preOffset = data.inputBufferReadOffset;
        const uint8_t preSubframes = currentFrameRemainingSubframes_;

        Decode(memory, &data);
        Consume(&ring, &data);

        if (!data.IsAnyInputBufferValid() || data.errorStatus == 4)
            break;

        // No offset progress and no frame produced: break instead of
        // spinning. (When subframes were pending, Decode intentionally
        // skipped while Consume drained -- that is progress.)
        if (preSubframes == 0 && data.inputBufferReadOffset == preOffset &&
            currentFrameRemainingSubframes_ == 0)
        {
            lucent::debug("xma", "context {}: decode stalled at offset {}",
                index_, preOffset);
            break;
        }
    }

    if (initial.IsAnyInputBufferValid())
    {
        data.outputBufferWriteOffset = ring.WriteOffset() / kOutputBytesPerBlock;
    }
    else if (data.outputBufferWriteOffset != data.outputBufferReadOffset)
    {
        // Starved of input: some titles use write==read as the stall signal
        // (the reference cites the NFS games), so make them equal.
        data.outputBufferWriteOffset = data.outputBufferReadOffset;
        data.outputBufferValid = 0;
    }

    // write caught read after real writes: the ring is FULL, tell the title
    // by invalidating the output buffer until it consumes.
    if (remainingSubframeBlocks_ == 0 && ring.Empty())
        data.outputBufferValid = 0;

    StoreContextMerged(data, initial, guest);
}

} // namespace gears
