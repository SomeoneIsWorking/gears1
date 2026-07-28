#include "xma_decode.h"

#include <lucent/log.h>

#if GEARS_HAVE_XMA_DECODER
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#endif

namespace gears
{

#if !GEARS_HAVE_XMA_DECODER

bool XmaDecoderAvailable() { return false; }
XmaFrameDecoder::~XmaFrameDecoder() = default;
void XmaFrameDecoder::Close() {}
void XmaFrameDecoder::Reset() {}
bool XmaFrameDecoder::Open(uint32_t, bool)
{
    lucent::error("xma", "this build has no XMA decoder"
        " (git submodule update --init extern/ffmpeg-xmaframes)");
    return false;
}
int XmaFrameDecoder::Decode(const uint8_t*, size_t, std::vector<float>&) { return -1; }

#else

bool XmaDecoderAvailable() { return true; }

XmaFrameDecoder::~XmaFrameDecoder()
{
    Close();
}

void XmaFrameDecoder::Close()
{
    if (frame_)
        av_frame_free(&frame_);
    if (packet_)
        av_packet_free(&packet_);
    if (context_)
        avcodec_free_context(&context_);
}

bool XmaFrameDecoder::Open(uint32_t sampleRate, bool stereo)
{
    Close();

    // XMAFRAMES exists only in the vendored fork. If this lookup fails the
    // build linked the wrong libavcodec, which is worth saying plainly rather
    // than reporting as a decode failure later.
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
    if (!codec)
    {
        lucent::error("xma", "libavcodec has no XMAFRAMES decoder -- this is the"
            " wrong libavcodec, not a decode problem");
        return false;
    }

    context_ = avcodec_alloc_context3(codec);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!context_ || !packet_ || !frame_)
    {
        lucent::error("xma", "out of memory allocating the decoder");
        Close();
        return false;
    }

    channels_ = stereo ? 2 : 1;
    sampleRate_ = sampleRate;
    context_->sample_rate = int(sampleRate);
    av_channel_layout_default(&context_->ch_layout, int(channels_));

    // The codec reports encoder priming as skip-samples side data, and by
    // default libavcodec DISCARDS them (frame 1 comes back 384 samples
    // instead of 512). The hardware emits every decoded sample; the title's
    // own bookkeeping assumes it. SKIP_MANUAL leaves the samples in and the
    // side data visible. Same flag, same reason as the reference
    // (xma_context_new.cc:910).
    context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

    if (const int rc = avcodec_open2(context_, codec, nullptr); rc < 0)
    {
        char message[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(rc, message, sizeof(message));
        lucent::error("xma", "avcodec_open2 failed: {}", message);
        Close();
        return false;
    }

    lucent::info("xma", "decoder open: {} at {} Hz", channels_ == 2 ? "stereo" : "mono",
                 sampleRate_);
    return true;
}

void XmaFrameDecoder::Reset()
{
    if (context_)
        avcodec_flush_buffers(context_);
}

int XmaFrameDecoder::Decode(const uint8_t* frame, size_t bytes, std::vector<float>& out)
{
    if (!context_ || !frame || bytes == 0)
        return -1;

    // av_packet_from_data wants ownership, and the frame lives in guest memory
    // we do not own, so the data is referenced rather than adopted. The codec
    // does not retain it past avcodec_send_packet for this decoder.
    packet_->data = const_cast<uint8_t*>(frame);
    packet_->size = int(bytes);

    if (const int rc = avcodec_send_packet(context_, packet_); rc < 0)
    {
        char message[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(rc, message, sizeof(message));
        lucent::debug("xma", "send_packet: {}", message);
        return -1;
    }

    const int rc = avcodec_receive_frame(context_, frame_);
    if (rc == AVERROR(EAGAIN))
        return 0; // wants another frame first, which is normal at the start
    if (rc < 0)
    {
        char message[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(rc, message, sizeof(message));
        lucent::debug("xma", "receive_frame: {}", message);
        return -1;
    }

    // The decoder produces planar float; the console's output ring is
    // interleaved, so the interleave happens here rather than being left for a
    // caller to get wrong.
    const int samples = frame_->nb_samples;
    const int planes = frame_->ch_layout.nb_channels;
    out.resize(size_t(samples) * size_t(planes));
    for (int channel = 0; channel < planes; ++channel)
    {
        const float* plane = reinterpret_cast<const float*>(frame_->extended_data[channel]);
        for (int i = 0; i < samples; ++i)
            out[size_t(i) * size_t(planes) + size_t(channel)] = plane[i];
    }
    return samples;
}

#endif

} // namespace gears
