// Replays a dumped XMA context through the runtime's REAL context protocol.
//
// Input is a GEARS_XMA_DUMP pair: ctxN.ctx (the 64-byte context as the title
// armed it) and ctxN.packets (its input buffer). This tool plays the TITLE's
// role -- kick, drain the output ring, advance the read offset, re-validate
// the output buffer -- against the exact xma_context.cpp the runtime links, so
// its output is the runtime's decode with the guest removed from the loop.
// Compared against the golden ffmpeg decode of the same dump with
// tools/xma_compare.py, this is the acceptance gate for the decoder.
//
// The guest is the slow part of that loop: a boot run takes minutes and the
// title chooses what to play. This replays a captured stream in about a
// second, which is what makes decode a debuggable subsystem rather than a
// listen-and-guess one. Same instrument shape as tools/frame_replay.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "byte_order.h"
#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_memory.h"
#include "xma_context.h"

namespace
{

// Guest addresses for the replay. Arbitrary but fixed: a committed context
// slot, an output ring, and the packet buffer, all far apart.
constexpr uint32_t kContextAddress = 0x00100000;
constexpr uint32_t kRingAddress = 0x00200000;
constexpr uint32_t kInputAddress = 0x40000000;

std::vector<uint8_t> ReadFile(const std::string &path)
{
    std::vector<uint8_t> data;
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return data;
    std::fseek(f, 0, SEEK_END);
    data.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(data.data(), 1, data.size(), f) != data.size())
        data.clear();
    std::fclose(f);
    return data;
}

void WriteWavHeader(std::FILE *f, uint32_t sampleRate, uint16_t channels, uint32_t dataBytes)
{
    const uint32_t byteRate = sampleRate * channels * 2;
    const uint16_t blockAlign = uint16_t(channels * 2);
    const uint32_t riffSize = 36 + dataBytes;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riffSize, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    const uint32_t fmtSize = 16;
    const uint16_t pcm = 1;
    std::fwrite(&fmtSize, 4, 1, f);
    std::fwrite(&pcm, 2, 1, f);
    std::fwrite(&channels, 2, 1, f);
    std::fwrite(&sampleRate, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&blockAlign, 2, 1, f);
    const uint16_t bits = 16;
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&dataBytes, 4, 1, f);
}

} // namespace

int main(int argc, char **argv)
{
    // Same prefix as the runtime, so GEARS_LUCENT_DEBUG=xma works here too.
    lucent::config::set_prefix("GEARS_");
    if (argc < 4)
    {
        lucent::error("xma", "usage: xma_replay <ctx> <packets> <out.wav>"
                             " [max-seconds]");
        return 2;
    }
    const std::string contextPath = argv[1];
    const std::string packetsPath = argv[2];
    const std::string outPath = argv[3];
    const double maxSeconds = argc > 4 ? std::atof(argv[4]) : 0.0;

    const std::vector<uint8_t> contextBytes = ReadFile(contextPath);
    const std::vector<uint8_t> packets = ReadFile(packetsPath);
    if (contextBytes.size() != 64)
    {
        lucent::error("xma", "{} is not a 64-byte context", contextPath);
        return 2;
    }
    if (packets.empty() || packets.size() % gears::xmaconst::kBytesPerPacket != 0)
    {
        lucent::error("xma", "{} is not a whole number of 2 KiB packets", packetsPath);
        return 2;
    }

    gears::GuestMemory memory;
    if (!memory.Reserve())
    {
        lucent::error("xma", "could not reserve guest memory");
        return 2;
    }
    if (!memory.Commit(kContextAddress, 0x1000) || !memory.Commit(kRingAddress, 0x10000) ||
        !memory.Commit(kInputAddress, uint32_t(packets.size())))
    {
        lucent::error("xma", "could not commit replay regions");
        return 2;
    }

    std::memcpy(memory.Translate<uint8_t>(kContextAddress), contextBytes.data(),
                contextBytes.size());
    std::memcpy(memory.Translate<uint8_t>(kInputAddress), packets.data(), packets.size());

    uint8_t *guestContext = memory.Translate<uint8_t>(kContextAddress);
    gears::XmaContextData data(guestContext);

    // Repoint the buffers at where this address space holds them. The dumped
    // context's own field values (packet counts, subframe pacing, block
    // count, offsets) are used untouched -- they ARE the test.
    const bool secondBufferAliasesFirst =
        data.inputBuffer1Valid && data.inputBuffer1Ptr == data.inputBuffer0Ptr;
    data.inputBuffer0Ptr = kInputAddress;
    if (secondBufferAliasesFirst)
    {
        data.inputBuffer1Ptr = kInputAddress;
    }
    else if (data.inputBuffer1Valid)
    {
        // The dump captured only buffer 0. Pretending buffer 1 exists would
        // decode unrelated memory, so replay buffer 0 alone and say so.
        lucent::warn("xma",
                     "context dumped with a second buffer at {:#x} that"
                     " was not captured; replaying buffer 0 only",
                     data.inputBuffer1Ptr);
        data.inputBuffer1Valid = 0;
        data.inputBuffer1Ptr = 0;
    }
    data.outputBufferPtr = kRingAddress;
    data.outputBufferReadOffset = 0;
    data.outputBufferWriteOffset = 0;
    data.outputBufferValid = 1;
    data.Store(guestContext);

    const uint32_t channels = data.isStereo ? 2 : 1;
    const uint32_t sampleRate =
        gears::xmaconst::kIdToSampleRate[std::min(uint32_t(data.sampleRate), 3u)];
    const uint32_t blockCount = data.outputBufferBlockCount;
    const uint64_t maxBytes =
        maxSeconds > 0.0 ? uint64_t(maxSeconds * sampleRate) * channels * 2 : ~0ULL;

    lucent::info("xma",
                 "replaying {}: {} packets, {} Hz {}, {}-block ring,"
                 " subframe count {}",
                 contextPath, packets.size() / gears::xmaconst::kBytesPerPacket, sampleRate,
                 channels == 2 ? "stereo" : "mono", blockCount, uint32_t(data.subframeDecodeCount));

    gears::XmaHwContext context(0);
    const uint8_t *ring = memory.Translate<uint8_t>(kRingAddress);

    std::vector<uint8_t> pcm;
    uint64_t kicks = 0;
    uint32_t idlePasses = 0;

    while (pcm.size() < maxBytes)
    {
        context.Work(memory, kContextAddress);
        ++kicks;

        gears::XmaContextData now(guestContext);

        // Drain the ring exactly as the title would: consume the blocks
        // between the read and write offsets, then hand the space back.
        bool drained = false;
        uint32_t readOffset = now.outputBufferReadOffset;
        while (readOffset != now.outputBufferWriteOffset)
        {
            const uint8_t *block =
                ring + size_t(readOffset) * gears::xmaconst::kOutputBytesPerBlock;
            pcm.insert(pcm.end(), block, block + gears::xmaconst::kOutputBytesPerBlock);
            readOffset = (readOffset + 1) % blockCount;
            drained = true;
        }
        if (drained)
        {
            now.outputBufferReadOffset = readOffset;
            now.outputBufferValid = 1;
            now.Store(guestContext);
            idlePasses = 0;
        }
        else if (++idlePasses > 2)
        {
            // Two honest chances after the last drain; a real title would
            // also re-kick after consuming. No progress twice with input
            // still queued is a protocol failure worth failing loudly on.
            break;
        }

        if (!now.inputBuffer0Valid && (secondBufferAliasesFirst || !now.inputBuffer1Valid))
        {
            // One full pass of the captured data. When buffer 1 aliased
            // buffer 0 (the title had queued the same data twice), stop at
            // the first pass: the golden reference is a single pass.
            break;
        }
    }

    gears::XmaContextData final(guestContext);
    const bool inputExhausted = !final.inputBuffer0Valid;

    // The ring holds big-endian samples -- guest byte order. The WAV wants
    // little-endian.
    for (size_t i = 0; i + 1 < pcm.size(); i += 2)
        std::swap(pcm[i], pcm[i + 1]);

    std::FILE *out = std::fopen(outPath.c_str(), "wb");
    if (!out)
    {
        lucent::error("xma", "could not open {}", outPath);
        return 2;
    }
    WriteWavHeader(out, sampleRate, uint16_t(channels), uint32_t(pcm.size()));
    std::fwrite(pcm.data(), 1, pcm.size(), out);
    std::fclose(out);

    const double seconds = double(pcm.size()) / (double(sampleRate) * channels * 2);
    lucent::info("xma",
                 "{}: {} bytes of PCM ({:.2f} s) in {} kicks, input {},"
                 " final read offset {} bits",
                 outPath, pcm.size(), seconds, kicks,
                 inputExhausted ? "exhausted" : "NOT exhausted",
                 uint32_t(final.inputBufferReadOffset));

    if (!inputExhausted && maxSeconds <= 0.0)
    {
        lucent::error("xma", "replay stalled before the input was consumed");
        return 1;
    }
    return 0;
}
