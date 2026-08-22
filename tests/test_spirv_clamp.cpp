// Tests for the fragment-output clamp.
//
// The property under test is invisible in every screenshot this project takes
// until it is wrong in a specific way: without the clamp, a shader that outputs a
// value above 1 into a fixed-point render target blends as if the target were HDR,
// and a translucent highlight comes out as a flat saturated slab. So these tests
// check the TRANSFORM, structurally -- that a clamp is inserted before each colour
// store, that the module stays well-formed, and above all that a module it cannot
// handle is REFUSED rather than half-transformed.

#include <cstdio>
#include <cstring>
#include <vector>

#include "spirv_clamp.h"
#include "test_fragment_spv.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char *what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

constexpr uint32_t kMagic = 0x07230203u;
constexpr uint16_t kOpExtInst = 12;
constexpr uint16_t kOpStore = 62;
constexpr uint32_t kNClamp = 81;

uint16_t Opcode(uint32_t w)
{
    return uint16_t(w & 0xFFFFu);
}
uint16_t Len(uint32_t w)
{
    return uint16_t(w >> 16);
}

// Walks the module and returns false if any instruction length is impossible --
// the cheapest structural check that a rebuild did not corrupt the stream.
bool WellFormed(const std::vector<uint32_t> &m)
{
    if (m.size() < 5 || m[0] != kMagic)
        return false;
    for (size_t i = 5; i < m.size();)
    {
        const uint16_t len = Len(m[i]);
        if (len == 0 || i + len > m.size())
            return false;
        i += len;
    }
    return true;
}

size_t CountNClamps(const std::vector<uint32_t> &m)
{
    size_t n = 0;
    for (size_t i = 5; i < m.size();)
    {
        const uint16_t len = Len(m[i]);
        if (len == 0 || i + len > m.size())
            break;
        if (Opcode(m[i]) == kOpExtInst && len >= 5 && m[i + 4] == kNClamp)
            ++n;
        i += len;
    }
    return n;
}

size_t CountStores(const std::vector<uint32_t> &m)
{
    size_t n = 0;
    for (size_t i = 5; i < m.size();)
    {
        const uint16_t len = Len(m[i]);
        if (len == 0 || i + len > m.size())
            break;
        if (Opcode(m[i]) == kOpStore)
            ++n;
        i += len;
    }
    return n;
}

// A clean test-owned fragment module with one vec4 colour output. It is built by
// glslangValidator from tests/shaders/spirv_clamp.frag, so it exercises the real
// module layout without embedding a title-derived shader.
void TestClampsARealModule()
{
    std::vector<uint32_t> m = gears::native::TestFragmentSpirv();
    const size_t storesBefore = CountStores(m);
    const uint32_t boundBefore = m[3];

    Check(CountNClamps(m) == 0, "the module has no NClamp before the transform");
    Check(gears::draw::ClampFragmentOutputs(m), "a real fragment module is handled");
    Check(WellFormed(m), "the rebuilt module is structurally well-formed");
    Check(CountNClamps(m) == 1, "exactly one clamp is inserted (one colour output)");
    Check(CountStores(m) == storesBefore,
          "no store is added or lost -- the clamp rewrites the stored VALUE");
    Check(m[3] > boundBefore, "the id bound grew to cover the new ids");
}

// THE CASE THAT MATTERS MOST. A caller that gets `false` uses the module
// unclamped; a caller that gets `true` from a mangled module renders garbage. So
// refusal has to be reliable, and it must not leave the input modified.
void TestRefusesWhatItCannotHandle()
{
    const std::vector<uint32_t> &good = gears::native::TestFragmentSpirv();

    std::vector<uint32_t> empty;
    Check(!gears::draw::ClampFragmentOutputs(empty), "an empty module is refused");

    std::vector<uint32_t> notSpirv{1, 2, 3, 4, 5, 6};
    const std::vector<uint32_t> notSpirvCopy = notSpirv;
    Check(!gears::draw::ClampFragmentOutputs(notSpirv), "a blob with the wrong magic is refused");
    Check(notSpirv == notSpirvCopy, "and is left exactly as it was");

    // A truncated instruction: the length field runs past the end.
    std::vector<uint32_t> truncated = good;
    truncated.resize(12);
    const std::vector<uint32_t> truncatedCopy = truncated;
    Check(!gears::draw::ClampFragmentOutputs(truncated),
          "a module whose last instruction runs past the end is refused");
    Check(truncated == truncatedCopy,
          "and is NOT left half-transformed -- the caller can still use it as it was");

    // A header with no instructions at all: nothing to clamp is a refusal, not a
    // silent success, because "clamped" would then be a claim about nothing.
    std::vector<uint32_t> headerOnly(good.begin(), good.begin() + 5);
    Check(!gears::draw::ClampFragmentOutputs(headerOnly),
          "a module with no fragment output is refused rather than reported clamped");
}

// Applying it twice must still produce a valid module -- the renderer caches per
// (shader, clamped), but a cache bug must degrade to redundant work, not corruption.
void TestSecondApplicationStaysValid()
{
    std::vector<uint32_t> m = gears::native::TestFragmentSpirv();
    Check(gears::draw::ClampFragmentOutputs(m), "first application succeeds");
    Check(gears::draw::ClampFragmentOutputs(m), "second application succeeds");
    Check(WellFormed(m), "and the module is still well-formed");
    Check(CountNClamps(m) == 2, "with the clamp applied twice, which is harmless");
}

// The 7e3 case: RGB must be left alone (it is the HDR range the widened host
// format exists to carry) and only alpha clamped. Structurally that is a different
// instruction sequence, so it gets its own check rather than riding on the other.
void TestAlphaOnlyModeTouchesOnlyAlpha()
{
    std::vector<uint32_t> m = gears::native::TestFragmentSpirv();
    const size_t storesBefore = CountStores(m);
    Check(gears::draw::ClampFragmentOutputs(m, gears::draw::ClampMode::kAlphaOnly),
          "alpha-only mode handles a real fragment module");
    Check(WellFormed(m), "and the rebuilt module is well-formed");
    Check(CountNClamps(m) == 1, "one clamp, on the alpha component");
    Check(CountStores(m) == storesBefore, "no store added or lost");

    // The distinguishing structural fact: alpha-only extracts and re-inserts a
    // component, the RGBA mode does not touch composites at all.
    auto countOp = [](const std::vector<uint32_t> &mod, uint16_t want)
    {
        size_t n = 0;
        for (size_t i = 5; i < mod.size();)
        {
            const uint16_t len = Len(mod[i]);
            if (len == 0 || i + len > mod.size())
                break;
            if (Opcode(mod[i]) == want)
                ++n;
            i += len;
        }
        return n;
    };
    const size_t inserts = countOp(m, 82 /*OpCompositeInsert*/);
    Check(inserts >= 1, "alpha-only re-inserts the clamped component");

    std::vector<uint32_t> rgba = gears::native::TestFragmentSpirv();
    const size_t insertsBefore = countOp(rgba, 82);
    Check(gears::draw::ClampFragmentOutputs(rgba, gears::draw::ClampMode::kRgba),
          "rgba mode still handled");
    Check(countOp(rgba, 82) == insertsBefore,
          "and rgba mode adds no composite insert -- the two modes really differ");
}

} // namespace

int main()
{
    TestClampsARealModule();
    TestAlphaOnlyModeTouchesOnlyAlpha();
    TestRefusesWhatItCannotHandle();
    TestSecondApplicationStaysValid();
    if (g_failures != 0)
    {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("spirv clamp: all checks passed\n");
    return 0;
}
