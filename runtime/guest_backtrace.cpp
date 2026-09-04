#include "guest_backtrace.h"

#include "guest_memory.h"

namespace gears
{
namespace
{

// The Xbox 360 stack-frame contract recovered from the executable is:
//
//     std  r28,-40(r1)      ... callee-saved registers, below the frame
//     stw  r12,-8(r1)       ... the RETURN ADDRESS, at -8 from the frame top
//     blr
//   caller then does:
//     stwu r1,-N(r1)        ... back chain stored at the new stack pointer
//
// So from any frame: the word at 0(sp) is the caller's frame top, and the
// return address into that caller is the word 8 below it. Functions that save
// the link register inline (`mflr r12; stw r12,-8(r1)`) use the same slot.
constexpr uint32_t kLinkRegisterSlot = 8;

// A guest stack is 1 MiB (see kStackSize in main.cpp) and frames only ever move
// UP as the walk unwinds. Both facts are the bounds check: the walk can never
// wander outside the stack it started on, which matters because most of the
// 4 GiB guest space is reserved but not committed and reading it would fault.
constexpr uint32_t kMaxStackSpan = 2 * 1024 * 1024;

bool InImage(uint32_t address)
{
    return address >= PPC_IMAGE_BASE && address < PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
}

uint32_t ReadWord(uint32_t address)
{
    return __builtin_bswap32(*Memory().Translate<uint32_t>(address));
}

} // namespace

std::vector<uint32_t> GuestBacktrace(uint32_t stackPointer, uint32_t lr, size_t maxFrames)
{
    std::vector<uint32_t> frames;
    if (InImage(lr))
        frames.push_back(lr);

    const uint32_t floor = stackPointer;
    uint32_t sp = stackPointer;
    while (frames.size() < maxFrames)
    {
        if (sp == 0 || (sp & 3) != 0 || sp < floor || sp - floor > kMaxStackSpan)
            break;

        const uint32_t caller = ReadWord(sp);
        // Strictly increasing: a back chain that does not move up the stack is
        // a corrupt or absent frame, and following it would loop forever.
        if (caller <= sp || caller - floor > kMaxStackSpan)
            break;

        const uint32_t returnAddress = ReadWord(caller - kLinkRegisterSlot);
        // A leaf frame never wrote this slot, so what is in it belongs to
        // whoever used the stack last. Dropping it rather than reporting it
        // keeps the trace honest -- a plausible-looking wrong address is worse
        // than a short trace.
        if (InImage(returnAddress) && (frames.empty() || frames.back() != returnAddress))
            frames.push_back(returnAddress);

        sp = caller;
    }
    return frames;
}

std::string FormatGuestBacktrace(uint32_t stackPointer, uint32_t lr, size_t maxFrames)
{
    const std::vector<uint32_t> frames = GuestBacktrace(stackPointer, lr, maxFrames);
    if (frames.empty())
        return "<no guest frames>";

    std::string out;
    char buffer[16];
    for (size_t i = 0; i < frames.size(); ++i)
    {
        if (i != 0)
            out += " <- ";
        snprintf(buffer, sizeof(buffer), "%#x", frames[i]);
        out += buffer;
    }
    return out;
}

} // namespace gears
