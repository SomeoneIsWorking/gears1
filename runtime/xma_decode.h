#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Forward-declared at global scope on purpose: written inside the namespace
// they would declare gears::AVCodecContext, a different type that happens to
// share a name, and the libavcodec calls would not match.
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace gears
{

// The XMA frame decoder.
//
// One reconstructed XMA frame in, one block of PCM out. That granularity is the
// whole point: the console's consumer paces itself per 128-sample subframe and
// tracks its position in the input as a BIT offset, so a decoder that buffers
// internally and hands back audio on its own schedule cannot be made to line up
// with it without inventing a compensation layer nobody has written. The
// vendored fork's AV_CODEC_ID_XMAFRAMES has no FIFO and no priming trim, and
// can be re-seeded at any frame boundary (see cmake/ffmpeg_xma.cmake).
//
// Reassembling frames out of the packet stream is NOT this class's job -- that
// is the context protocol, and it is where the hard cases live (frames spanning
// packets, a frame's own 15-bit length header straddling a packet boundary).
// This is only the codec.
class XmaFrameDecoder
{
public:
    ~XmaFrameDecoder();

    // Reports failure rather than degrading: a decoder that silently does
    // nothing is indistinguishable from the silence this is meant to fix.
    bool Open(uint32_t sampleRate, bool stereo);
    bool IsOpen() const { return context_ != nullptr; }

    // Decodes one frame. `out` receives interleaved float samples; the return
    // is the number of samples PER CHANNEL, 0 if the codec wants more data
    // (which is normal for the first frame), and -1 on a real error.
    int Decode(const uint8_t* frame, size_t bytes, std::vector<float>& out);

    void Reset();

    uint32_t channels() const { return channels_; }
    uint32_t sampleRate() const { return sampleRate_; }

private:
    void Close();

    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    uint32_t channels_ = 0;
    uint32_t sampleRate_ = 0;
};

// True when the runtime was built with the decoder available at all. Kept
// explicit so "no decoder in this build" and "decoder failed" stay different
// answers in the log.
bool XmaDecoderAvailable();

} // namespace gears
