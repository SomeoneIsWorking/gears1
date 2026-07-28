#include "xma.h"

#include <atomic>
#include <bitset>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string>

#include <byteswap.h>

#include <lucent/config.h>
#include <lucent/log.h>

#include "guest_heap.h"
#include "xma_decode.h"

namespace gears
{

namespace
{

// The register window, and the indices within it that matter. An index is a
// word offset, so a register's address is base + index * 4 (Xenia's own
// arithmetic: r = (addr & 0xFFFF) / 4).
constexpr uint32_t kXmaRegisterBase = 0x7FEA0000;
constexpr uint32_t kXmaRegisterWindow = 0x00010000;
constexpr uint32_t kRegContextArrayAddress = 0x0600;
constexpr uint32_t kRegNextContextIndex = 0x0607;
constexpr uint32_t kRegContext0Kick = 0x0650;
constexpr uint32_t kRegContext0Lock = 0x0690;
constexpr uint32_t kRegContext0Clear = 0x06A0;
constexpr uint32_t kContextGroupCount = 10; // 10 registers of 32 bits each

// 320 contexts of 64 bytes, which is the console's array and therefore the
// range of indices the title's bitmaps can address.
constexpr uint32_t kContextCount = 320;
constexpr uint32_t kContextSize = 64;
constexpr uint32_t kPacketSize = 2048;

uint32_t g_contextArray = 0;
GuestMemory* g_memory = nullptr;

std::mutex g_contextMutex;
std::bitset<kContextCount> g_contextInUse;

// Registers are read by the title with `lwbrx` -- a byte-reversed load, because
// device registers are little-endian -- so a value published here is stored in
// the host's own byte order rather than the guest's. (Confirmed at
// sub_825E8FE0, which reads the context-array register exactly that way, and by
// the writes the title makes: they read back correctly only when interpreted
// little-endian.)
uint32_t* Register(GuestMemory& memory, uint32_t index)
{
    return memory.Translate<uint32_t>(kXmaRegisterBase + index * 4);
}

// Kicks arrive as WRITES, from the device-store hook, so every one is seen.
// Nothing decodes them yet; what this does is make them visible and count them
// honestly. Without it, a title that asks for decode and a title that never
// asks look identical -- the same ambiguity that hid the audio blocker.
std::atomic<uint64_t> g_kicks{0};

// GEARS_XMA_DUMP=<dir> writes a kicked context and the XMA data behind it, once
// per context, so the bitstream can be decoded OFFLINE before any decoder
// exists in the runtime.
//
// That order is deliberate. An in-runtime decoder that produces silence could
// be failing at the bitstream, at the context bookkeeping, at the output ring,
// or at the premise that this is even XMA -- and those look identical from the
// pump. Decoding the dump with a known-good tool separates the premise from the
// plumbing, and gives a reference the runtime's own output must later match.
constexpr uint32_t kIdToSampleRate[4] = {24000, 32000, 44100, 48000};

struct ContextFields
{
    uint32_t packetCount;
    bool inputValid;
    bool isStereo;
    uint32_t sampleRate;
    uint32_t inputBuffer;
};

ContextFields ReadContext(GuestMemory& memory, uint32_t pointer)
{
    const uint32_t* words = memory.Translate<uint32_t>(pointer);
    auto dword = [words](uint32_t i) { return ByteSwap(words[i]); };

    ContextFields out{};
    out.packetCount = dword(0) & 0xFFF;
    out.inputValid = ((dword(0) >> 20) & 1) != 0;
    out.isStereo = ((dword(1) >> 29) & 1) != 0;
    out.sampleRate = kIdToSampleRate[(dword(1) >> 27) & 3];
    out.inputBuffer = dword(5);
    return out;
}

void DumpContext(GuestMemory& memory, uint32_t index, uint32_t pointer)
{
    const std::string& dir = lucent::config::text("XMA_DUMP");
    if (dir.empty())
        return;

    static std::mutex dumpMutex;
    static std::set<uint32_t> dumped;
    std::lock_guard<std::mutex> guard(dumpMutex);
    if (!dumped.insert(index).second)
        return;

    const ContextFields fields = ReadContext(memory, pointer);
    if (!fields.inputValid || fields.inputBuffer == 0 || fields.packetCount == 0)
    {
        lucent::warn("xma", "context {} kicked with no input buffer to dump", index);
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const uint32_t bytes = fields.packetCount * kPacketSize;

    // The context itself as well as the data: the header fields are what a
    // reader needs to interpret the stream, and reconstructing them by hand
    // from a log line is how transcription errors get in.
    const std::string stem = dir + "/ctx" + std::to_string(index);
    if (std::FILE* f = std::fopen((stem + ".ctx").c_str(), "wb"))
    {
        std::fwrite(memory.Translate<uint8_t>(pointer), 1, kContextSize, f);
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen((stem + ".packets").c_str(), "wb"))
    {
        std::fwrite(memory.Translate<uint8_t>(fields.inputBuffer), 1, bytes, f);
        std::fclose(f);
    }
    lucent::info("xma", "context {} dumped to {}.packets: {} packets ({} bytes),"
        " {} at {} Hz, input buffer {:#x}", index, stem, fields.packetCount, bytes,
        fields.isStereo ? "stereo" : "mono", fields.sampleRate, fields.inputBuffer);
}

// Opening the codec for a context is not decoding it, and this does not pretend
// otherwise. It answers one question the build alone cannot: whether the
// libavcodec actually linked into this binary has the per-frame XMA decoder.
// "The wrong libavcodec" and "the bitstream is bad" produce the same silence
// later, so they are separated now, at the point where the answer is cheap.
std::unordered_map<uint32_t, XmaFrameDecoder> g_decoders;
std::mutex g_decoderMutex;

void OpenDecoderOnce(GuestMemory& memory, uint32_t index, uint32_t pointer)
{
    std::lock_guard<std::mutex> guard(g_decoderMutex);
    if (g_decoders.count(index))
        return;

    const ContextFields fields = ReadContext(memory, pointer);
    if (!fields.inputValid)
        return;

    XmaFrameDecoder& decoder = g_decoders[index];
    if (!decoder.Open(fields.sampleRate, fields.isStereo))
        lucent::warn("xma", "context {} has no decoder behind it", index);
}

void ReportContextBits(const char* what, uint32_t group, uint32_t bits)
{
    static std::atomic<uint64_t> s_reported{0};
    while (bits)
    {
        const uint32_t bit = uint32_t(__builtin_ctz(bits));
        bits &= bits - 1;
        const uint32_t index = group * 32 + bit;
        // A streaming title kicks continuously, so a line per kick would drown
        // the run it is meant to explain. The totals are reported at exit.
        if (s_reported.fetch_add(1) < 8)
            lucent::info("xma", "context {} {}, and nothing decodes it", index, what);
    }
}

} // namespace

bool SetupXmaRegisters(GuestMemory& memory)
{
    // The console's kernel allocates the array as uncached physical memory and
    // writes its physical address to the register. Virtual and physical are the
    // same view in this runtime (MmGetPhysicalAddress is the identity), so the
    // address the title reads from the register is the same one it will see
    // back from XMACreateContext -- which is what its index arithmetic needs.
    g_memory = &memory;
    uint32_t size = kContextCount * kContextSize;
    g_contextArray = PhysicalHeap().Allocate(0, size, kMemCommit);
    if (g_contextArray == 0)
    {
        lucent::error("xma", "could not allocate the {}-context array", kContextCount);
        return false;
    }
    if ((g_contextArray & 0x3F) != 0)
    {
        // The title recovers an index by subtracting the base and shifting by
        // 6, so a misaligned base makes every index wrong by a fraction of a
        // slot. Better to fail loudly than to decode into the wrong context.
        lucent::error("xma", "context array at {:#x} is not 64-byte aligned",
            g_contextArray);
        return false;
    }

    std::memset(memory.Translate<uint8_t>(g_contextArray), 0, size);
    *Register(memory, kRegContextArrayAddress) = g_contextArray;
    *Register(memory, kRegNextContextIndex) = 1;

    lucent::info("xma", "context array at {:#x} ({} x {} bytes), published in"
        " register {:#x}", g_contextArray, kContextCount, kContextSize,
        kRegContextArrayAddress);

    return true;
}

uint32_t AllocateXmaContext()
{
    std::lock_guard<std::mutex> guard(g_contextMutex);
    if (g_contextArray == 0)
        return 0;
    for (uint32_t i = 0; i < kContextCount; ++i)
    {
        if (g_contextInUse[i])
            continue;
        g_contextInUse[i] = true;
        const uint32_t pointer = g_contextArray + i * kContextSize;
        lucent::debug("xma", "context {} allocated at {:#x}", i, pointer);
        return pointer;
    }
    lucent::error("xma", "all {} contexts are in use", kContextCount);
    return 0;
}

void ReleaseXmaContext(uint32_t guestPointer)
{
    std::lock_guard<std::mutex> guard(g_contextMutex);
    if (g_contextArray == 0 || guestPointer < g_contextArray)
        return;
    const uint32_t offset = guestPointer - g_contextArray;
    if (offset % kContextSize != 0 || offset / kContextSize >= kContextCount)
    {
        lucent::warn("xma", "release of {:#x}, which is not a context in the array",
            guestPointer);
        return;
    }
    g_contextInUse[offset / kContextSize] = false;
}

bool OnXmaRegisterStore(uint32_t address, uint32_t value)
{
    if (address < kXmaRegisterBase || address >= kXmaRegisterBase + kXmaRegisterWindow)
        return false;

    // The value reaching the hook is what a big-endian store would put in
    // memory; the registers are little-endian, so the register's value is the
    // byte-reversed form. (The title writes these with stwbrx, which the
    // recompiler turns into a byte-swapped argument to this same store -- so
    // reversing here recovers exactly what it meant.)
    const uint32_t index = (address & 0xFFFF) / 4;
    const uint32_t reg = __builtin_bswap32(value);

    if (index >= kRegContext0Kick && index < kRegContext0Kick + kContextGroupCount)
    {
        const uint32_t group = index - kRegContext0Kick;
        const uint64_t kicks =
            g_kicks.fetch_add(uint32_t(__builtin_popcount(reg))) + __builtin_popcount(reg);
        ReportContextBits("kicked", group, reg);
        for (uint32_t bits = reg; bits; bits &= bits - 1)
        {
            const uint32_t index = group * 32 + uint32_t(__builtin_ctz(bits));
            const uint32_t pointer = g_contextArray + index * kContextSize;
            DumpContext(*g_memory, index, pointer);
            OpenDecoderOnce(*g_memory, index, pointer);
        }
        // Reported as it goes rather than at exit. These runs end by being
        // killed -- the capture script sends SIGKILL and atexit never sees it --
        // so an exit summary is a summary nobody ever reads.
        if (kicks % 1000 == 0)
            lucent::info("xma", "{} context kicks asked for, none decoded", kicks);
    }
    else if (index >= kRegContext0Lock && index < kRegContext0Lock + kContextGroupCount)
    {
        ReportContextBits("locked", index - kRegContext0Lock, reg);
    }
    else if (index >= kRegContext0Clear && index < kRegContext0Clear + kContextGroupCount)
    {
        ReportContextBits("cleared", index - kRegContext0Clear, reg);
    }
    else
    {
        lucent::debug("xma", "register {:#x} <- {:#x}", index, reg);
    }
    return true;
}

} // namespace gears
