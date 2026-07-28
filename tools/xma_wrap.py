#!/usr/bin/env python3
"""Wrap a dumped XMA packet stream in a RIFF header so a decoder can read it.

The runtime dumps what the title queued for the hardware decoder
(GEARS_XMA_DUMP=<dir>): the 64-byte context and the raw 2 KB packets its input
buffer points at. Those packets are the bitstream, with no container around
them -- the console needs none, because the context IS the container.

This exists so the bitstream can be decoded OFFLINE, before any decoder exists
in the runtime. An in-runtime decoder that produces silence could be failing at
the bitstream, at the context bookkeeping, at the output ring, or at the premise
that this is XMA at all, and from the audio pump those look identical. Decoding
the dump with a known-good tool separates the premise from the plumbing, and
produces a reference the runtime's own decoder must later match.

    tools/xma_wrap.py scratch/bin/xma/ctx1
    ffmpeg -i scratch/bin/xma/ctx1.xma scratch/wav/ctx1.wav

The header is a WAVEFORMATEX of tag 0x0166 carrying a 34-byte XMA2WAVEFORMATEX
as extradata. The decoder takes its channel count from that structure's
ChannelMask and NOT from the WAVEFORMATEX -- filling in only the latter yields
"0 channels" and a refusal to open the decoder.

Verified on the title's own streams: the 1780-packet context decodes to
2 min 22 s of 44.1 kHz stereo (mean -22.2 dB, peak -2.9 dB) and the 55-packet
context to 1.17 s at 24 kHz (mean -15.4 dB, peak -1.1 dB), both without a
decoder error.
"""
import argparse
import pathlib
import struct
import sys

PACKET_SIZE = 2048
ID_TO_SAMPLE_RATE = (24000, 32000, 44100, 48000)


def read_context(path):
    """The fields of XMA_CONTEXT_DATA this needs, as big-endian dwords."""
    data = path.read_bytes()
    if len(data) < 64:
        sys.exit(f"{path}: expected a 64-byte context, got {len(data)}")
    d = struct.unpack(">16I", data[:64])
    return {
        "packet_count": d[0] & 0xFFF,
        "input_valid": bool((d[0] >> 20) & 1),
        "is_stereo": bool((d[1] >> 29) & 1),
        "sample_rate": ID_TO_SAMPLE_RATE[(d[1] >> 27) & 3],
        "subframe_decode_count": (d[1] >> 20) & 0xF,
        "loop_count": (d[0] >> 12) & 0xFF,
    }


def xma2_extradata(channels, sample_rate, packets, loop_count):
    """XMA2WAVEFORMATEX, the 34-byte form carried as WAVEFORMATEX extradata.

    The decoder takes the channel count from ChannelMask, not from the
    WAVEFORMATEX -- setting only the latter yields "0 channels" and a refusal to
    open, which is how this layout got pinned down rather than guessed.

        WORD  NumStreams      DWORD ChannelMask     DWORD SamplesEncoded
        DWORD BytesPerBlock   DWORD PlayBegin       DWORD PlayLength
        DWORD LoopBegin       DWORD LoopLength      BYTE  LoopCount
        BYTE  EncoderVersion  WORD  BlockCount
    """
    channel_mask = 0x3 if channels == 2 else 0x4  # L+R, or centre for mono
    total_bytes = packets * PACKET_SIZE
    return struct.pack(
        "<HIIIIIIIBBH",
        1,             # NumStreams: one stream carrying every channel, which is
                       # what a context describes
        channel_mask,  # ChannelMask -- the decoder's source of channel count
        0,             # SamplesEncoded: unknown here, and unused for decoding
        total_bytes,   # BytesPerBlock: the whole dump as one block
        0,             # PlayBegin
        0,             # PlayLength
        0,             # LoopBegin
        0,             # LoopLength
        loop_count,    # LoopCount, straight from the context
        4,             # EncoderVersion
        1,             # BlockCount
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stem", help="dump stem, e.g. scratch/bin/xma/ctx1"
                                 " (reads <stem>.ctx and <stem>.packets)")
    ap.add_argument("--out", help="output path (default <stem>.xma)")
    args = ap.parse_args()

    stem = pathlib.Path(args.stem)
    context = read_context(stem.with_suffix(".ctx"))
    packets = stem.with_suffix(".packets").read_bytes()

    if len(packets) % PACKET_SIZE:
        # Not fatal, but it means the dump and the context disagree, and a
        # truncated final packet is exactly the kind of thing that looks like a
        # decoder bug later.
        print(f"warning: {len(packets)} bytes is not a whole number of"
              f" {PACKET_SIZE}-byte packets", file=sys.stderr)
    if len(packets) != context["packet_count"] * PACKET_SIZE:
        print(f"warning: context says {context['packet_count']} packets,"
              f" the dump holds {len(packets) // PACKET_SIZE}", file=sys.stderr)

    channels = 2 if context["is_stereo"] else 1
    rate = context["sample_rate"]
    extradata = xma2_extradata(channels, rate,
                               len(packets) // PACKET_SIZE, context["loop_count"])

    fmt = struct.pack("<HHIIHHH", 0x0166, channels, rate,
                      rate * channels * 2,  # nominal byte rate
                      PACKET_SIZE,          # block align: one packet
                      16,                   # bits per sample of the OUTPUT
                      len(extradata)) + extradata

    body = (b"WAVE"
            + b"fmt " + struct.pack("<I", len(fmt)) + fmt
            + b"data" + struct.pack("<I", len(packets)) + packets)
    out = pathlib.Path(args.out) if args.out else stem.with_suffix(".xma")
    out.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)

    print(f"{out}: {len(packets) // PACKET_SIZE} packets, {channels} ch,"
          f" {rate} Hz, subframes {context['subframe_decode_count']},"
          f" loops {context['loop_count']}")


if __name__ == "__main__":
    main()
