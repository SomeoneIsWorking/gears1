// What a draw fetches. gpu_draw_vertexfetch.h says why the range matters twice
// over -- as the upload list and as the reach census.

#include "gpu_draw_vertexfetch.h"

#include <bit>
#include <algorithm>
#include <map>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <lucent/config.h>
#include <lucent/log.h>

#include "gpu_draw_formats.h"

namespace gears::draw
{

namespace
{

uint64_t Fnv1a64(const uint8_t *bytes, size_t size)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= bytes[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// The two knobs that name draws by their diag index -- GEARS_DRAW_VDUMP and
// GEARS_DRAW_VS_CONSTS -- share this, and it exists for the negative rather
// than the positive. `if (index == want) print()` prints NOTHING when the index
// names no draw in the frame, and nothing is exactly what a draw with nothing
// to show prints: an off-by-one, a typo, or an index read off a table from a
// different capture all read as an answer. A selection carries its denominator
// instead, so a run that matched nobody says so.
class DrawSelection
{
  public:
    DrawSelection(const char *knob, const std::string &spec) : knob_(knob)
    {
        for (size_t i = 0; i < spec.size();)
        {
            size_t j = spec.find(',', i);
            if (j == std::string::npos)
                j = spec.size();
            if (const std::string tok = spec.substr(i, j - i); !tok.empty())
            {
                char *end = nullptr;
                const long v = std::strtol(tok.c_str(), &end, 0);
                if (end && *end == '\0' && v >= 0)
                    want_.push_back(uint32_t(v));
                else
                    // A token we could not parse must not be silently dropped:
                    // a filter that quietly ignores what it cannot match turns
                    // a broken instrument into a clean bill of health.
                    bad_.push_back(tok);
            }
            i = j + 1;
        }
    }

    bool Active() const { return !want_.empty() || !bad_.empty(); }

    // Call for EVERY draw the knob could have selected, hit or miss -- that is
    // what makes the denominator real rather than assumed.
    bool Offer(uint32_t diagIndex)
    {
        ++offered_;
        lowest_ = std::min(lowest_, diagIndex);
        highest_ = std::max(highest_, diagIndex);
        const bool hit = std::find(want_.begin(), want_.end(), diagIndex) != want_.end();
        matched_ += hit;
        return hit;
    }

    void Report()
    {
        if (Active())
        {
            lucent::Line l;
            l.add("{}: matched {} of {} draws offered", knob_, matched_, offered_);
            if (offered_)
                l.add(" (diag indices {}..{} this frame)", lowest_, highest_);
            for (uint32_t w : want_)
                if (offered_ && (w < lowest_ || w > highest_))
                    l.add("; {} IS OUT OF RANGE for this frame", w);
            for (const std::string &b : bad_)
                l.add("; '{}' is not a draw index and was ignored", b);
            if (matched_ == 0 || !bad_.empty())
                l.flush(lucent::Level::Warn, "draw");
            else
                l.flush(lucent::Level::Info, "draw");
        }
        offered_ = matched_ = 0;
        lowest_ = std::numeric_limits<uint32_t>::max();
        highest_ = 0;
    }

  private:
    const char *knob_;
    std::vector<uint32_t> want_;
    std::vector<std::string> bad_;
    uint32_t offered_ = 0, matched_ = 0;
    uint32_t lowest_ = std::numeric_limits<uint32_t>::max(), highest_ = 0;
};

DrawSelection &VdumpSelection()
{
    static DrawSelection s("GEARS_DRAW_VDUMP", lucent::config::text("DRAW_VDUMP"));
    return s;
}

DrawSelection &VsConstSelection()
{
    static DrawSelection s("GEARS_DRAW_VS_CONSTS", lucent::config::text("DRAW_VS_CONSTS"));
    return s;
}

} // namespace

void ReportDrawSelections()
{
    VdumpSelection().Report();
    VsConstSelection().Report();
}

void CollectFetchRanges(const uint32_t *R, const FrameDrawInputs &in, const ShaderXlate &vsX,
                        const ShaderXlate &psX, FrameCensus &CN,
                        std::vector<std::pair<uint64_t, uint64_t>> &fetchRanges)
{
    bool anyPast = false, anyBinding = false;
    for (const auto &vb : vsX.vertexBindings)
    {
        const uint32_t fc = vb.fetchConstant & 95;
        const uint32_t d0 = R[0x4800 + fc * 2];
        const uint32_t d1 = R[0x4800 + fc * 2 + 1];
        if ((d0 & 3) != 3 /*kVertex*/)
            continue;
        anyBinding = true;
        const uint64_t begin = uint64_t(d0) & ~uint64_t(3);
        const uint64_t end = begin + ((d1 >> 2) & 0xFFFFFF) * 4ull;
        CN.vfHighestByte =
            std::max<uint32_t>(CN.vfHighestByte, uint32_t(std::min<uint64_t>(end, 0xFFFFFFFFull)));
        if (end > in.guestPhysicalMirrorBytes)
            anyPast = true;
        // This is the range the GPU will fetch from, so it is also
        // exactly the range that has to be uploaded.
        else if (end > begin)
            fetchRanges.emplace_back(begin, end);
    }
    if (anyBinding)
        (anyPast ? CN.vfDrawsPastMirror : CN.vfDrawsInMirror) += 1;
    // A pixel shader may carry vertex fetches of its own. They are not
    // part of the geometry-reach census (which is about whether this
    // draw's GEOMETRY resolves), but they are still memory the GPU will
    // read, so they still have to be uploaded.
    for (const auto &vb : psX.vertexBindings)
    {
        const uint32_t fc = vb.fetchConstant & 95;
        const uint32_t d0 = R[0x4800 + fc * 2];
        const uint32_t d1 = R[0x4800 + fc * 2 + 1];
        if ((d0 & 3) != 3 /*kVertex*/)
            continue;
        const uint64_t begin = uint64_t(d0) & ~uint64_t(3);
        const uint64_t end = begin + ((d1 >> 2) & 0xFFFFFF) * 4ull;
        if (end > begin && end <= in.guestPhysicalMirrorBytes)
            fetchRanges.emplace_back(begin, end);
    }
}

void DumpVertices(const uint32_t *R, const FrameDrawInputs &in, const FrameDrawItem &draw,
                  const ShaderXlate &vsX, const ShaderXlate &psX, uint32_t issued,
                  uint32_t diagIndex, uint64_t vsHash, uint32_t vertexCount,
                  const std::vector<uint8_t> *systemConstants,
                  const std::vector<uint8_t> *vertexConstants)
{
    // GEARS_DRAW_VDUMP_VS=<16-hex vertex shader hash>[:<min>[-<max>]] selects
    // by SHADER AND VERTEX COUNT instead of by diag index. A diag index is not
    // stable between runs of this title -- the same index named a 19,776-vertex
    // shadow volume in one run and an 866-vertex draw of a different shader in
    // the next -- so a dump aimed at one is aimed at nothing in particular.
    // Pairs with GEARS_DRAW_FRAME_NEEDS, which guarantees the frame contains it.
    struct VNeed
    {
        uint64_t hash = 0;
        uint32_t lo = 0;
        uint32_t hi = 0xFFFFFFFFu;
    };
    static const VNeed vneed = []
    {
        VNeed n;
        const std::string &t = lucent::config::text("DRAW_VDUMP_VS");
        if (t.empty())
            return n;
        const size_t colon = t.find(':');
        const std::string hex = t.substr(0, colon);
        char *end = nullptr;
        n.hash = std::strtoull(hex.c_str(), &end, 16);
        if (end == hex.c_str() || n.hash == 0)
        {
            lucent::warn("draw",
                         "GEARS_DRAW_VDUMP_VS: cannot parse '{}' as a"
                         " 16-hex vertex shader hash. NOTHING will be dumped by it",
                         hex);
            n.hash = 0;
            return n;
        }
        if (colon != std::string::npos)
        {
            const char *q = t.c_str() + colon + 1;
            char *e2 = nullptr;
            n.lo = uint32_t(std::strtoul(q, &e2, 10));
            if (e2 && *e2 == '-')
                n.hi = uint32_t(std::strtoul(e2 + 1, nullptr, 10));
        }
        lucent::info("draw",
                     "vertex dump armed for shader {:#018x} with {}..{}"
                     " vertices -- every draw that matches, not a diag index",
                     n.hash, n.lo, n.hi == 0xFFFFFFFFu ? std::string("any") : std::to_string(n.hi));
        return n;
    }();
    const bool shaderMatch = vneed.hash != 0 && vsHash == vneed.hash && vertexCount >= vneed.lo &&
                             vertexCount <= vneed.hi;
    DrawSelection &sel = VdumpSelection();
    if (!sel.Active() && !shaderMatch)
        return;
    if (shaderMatch || sel.Offer(diagIndex))
    {
        // The first 48 bytes are every system-constant field that can move a
        // vertex: flags, index-load address/endian/base, NDC scale and offset
        // (plus point-size padding). Print RAW WORDS so float formatting cannot
        // hide one ULP. Both renderers use the same Xenia ABI.
        if (systemConstants && systemConstants->size() >= 48)
        {
            uint32_t sw[12];
            std::memcpy(sw, systemConstants->data(), sizeof(sw));
            lucent::Line sl;
            sl.add("  draw {} geometry system[0..11]", issued);
            for (uint32_t w : sw)
                sl.add(" {:08x}", w);
            sl.flush(lucent::Level::Info, "draw");
            lucent::info("draw",
                         "  draw {} geometry system bytes {} hash"
                         " {:016x}",
                         issued, systemConstants->size(),
                         Fnv1a64(systemConstants->data(), systemConstants->size()));
        }
        else
            lucent::warn("draw",
                         "  draw {} geometry system constants missing"
                         " or shorter than 48 bytes; NO transform-state comparison is"
                         " possible",
                         issued);
        if (systemConstants && systemConstants->size() >= 112)
        {
            uint32_t sw[4];
            std::memcpy(sw, systemConstants->data() + 64, sizeof(sw));
            lucent::info("draw",
                         "  draw {} texture signs[0..3] {:08x} {:08x}"
                         " {:08x} {:08x}",
                         issued, sw[0], sw[1], sw[2], sw[3]);
            std::memcpy(sw, systemConstants->data() + 96, sizeof(sw));
            lucent::info("draw",
                         "  draw {} texture swizzles[0..3] {:08x}"
                         " {:08x} {:08x} {:08x}",
                         issued, sw[0], sw[1], sw[2], sw[3]);
        }
        else
            lucent::warn("draw",
                         "  draw {} system constants shorter than 112"
                         " bytes; NO texture sign/swizzle comparison is"
                         " possible",
                         issued);
        if (vertexConstants && !vertexConstants->empty())
        {
            lucent::info("draw",
                         "  draw {} packed vertex constants bytes {}"
                         " hash {:016x}",
                         issued, vertexConstants->size(),
                         Fnv1a64(vertexConstants->data(), vertexConstants->size()));
            const size_t vecCount = vertexConstants->size() / 16;
            for (size_t first = 0; first < vecCount; first += 16)
            {
                lucent::Line vl;
                vl.add("  draw {} packed vertex constants c{}..c{}", issued, first,
                       std::min(first + 15, vecCount - 1));
                const size_t end = std::min(first + 16, vecCount);
                for (size_t i = first; i < end; ++i)
                {
                    uint32_t words[4];
                    std::memcpy(words, vertexConstants->data() + i * 16, 16);
                    vl.add(" c{}={:08x},{:08x},{:08x},{:08x}", i, words[0], words[1], words[2],
                           words[3]);
                }
                vl.flush(lucent::Level::Info, "draw");
            }
            if (vertexConstants->size() % 16 != 0)
                lucent::warn("draw",
                             "  draw {} packed vertex constants have"
                             " {} trailing byte(s); the vec4 dump is INCOMPLETE",
                             issued, vertexConstants->size() % 16);
        }
        else
            lucent::warn("draw",
                         "  draw {} packed vertex constants are"
                         " EMPTY; NO bound-UBO comparison is possible",
                         issued);
        lucent::info("draw",
                     "  draw {} translated VS bytes {} hash {:016x};"
                     " PS bytes {} hash {:016x}",
                     issued, vsX.spirv.size(), Fnv1a64(vsX.spirv.data(), vsX.spirv.size()),
                     psX.spirv.size(), Fnv1a64(psX.spirv.data(), psX.spirv.size()));
        // Stored depth depends on both the sampled texture inputs and the bound
        // pixel constants. Equal raster counts can therefore still produce a
        // sparse depth map. Keep both input sets beside the geometry evidence.
        for (uint32_t pc : {0u, 1u, 255u})
        {
            lucent::Line pl;
            pl.add("  draw {} pixel c{}", issued, pc);
            for (uint32_t c = 0; c < 4; ++c)
                pl.add(" {:08x}", R[0x4400 + pc * 4 + c]);
            pl.flush(lucent::Level::Info, "draw");
        }
        for (const ShaderTextureBinding &tb : psX.textures)
        {
            const uint32_t fc = tb.fetchConstant & 31;
            const uint32_t *fetch = R + 0x4800 + fc * 6;
            lucent::Line tl;
            tl.add("  draw {} pixel fetch fc {}", issued, fc);
            for (uint32_t w = 0; w < 6; ++w)
                tl.add(" {:08x}", fetch[w]);
            GuestTexture gt;
            if (DecodeGuestTexture(fetch, in.guestBase, in.guestWindowBytes, true, gt) &&
                gt.skipReason == nullptr && gt.baseGuestExtentBytes)
                tl.add(" base {:08x} bytes {} rawhash {:016x}", gt.baseAddress,
                       gt.baseGuestExtentBytes,
                       Fnv1a64(in.guestBase + gt.baseAddress, gt.baseGuestExtentBytes))
                    .add(" mips {:08x} bytes {} rawhash {:016x}", gt.mipAddress,
                         gt.mipGuestExtentBytes,
                         Fnv1a64(in.guestBase + gt.mipAddress, gt.mipGuestExtentBytes))
                    .add(" uploadbytes {} uploadhash {:016x}", gt.data.size(),
                         Fnv1a64(gt.data.data(), gt.data.size()));
            else
                tl.add(" NO raw texture hash (fetch absent, unsupported, or"
                       " outside the guest window)");
            tl.flush(lucent::Level::Info, "draw");
            for (uint32_t level = 0; level < gt.levels.size(); ++level)
            {
                const GuestTextureLevel &decoded = gt.levels[level];
                lucent::info("draw",
                             "  draw {} pixel fetch fc {} level {}"
                             " uploadbytes {} uploadhash {:016x}",
                             issued, fc, level, decoded.dataSize,
                             Fnv1a64(gt.data.data() + decoded.dataOffset, decoded.dataSize));
            }
        }
        for (const ShaderSamplerBinding &sb : psX.samplers)
        {
            GuestSamplerState gs;
            if (DeriveSamplerState(R + 0x4800 + (sb.fetchConstant & 31) * 6, sb, gs))
                lucent::info("draw",
                             "  draw {} pixel sampler fc {} filters"
                             " {}/{}/{} clamp {}/{}/{} aniso {}",
                             issued, sb.fetchConstant & 31, gs.magFilter, gs.minFilter,
                             gs.mipFilter, gs.clamp[0], gs.clamp[1], gs.clamp[2], gs.anisoMax);
            else
                lucent::warn("draw",
                             "  draw {} pixel sampler fc {} could not"
                             " be derived; NO sampler comparison is possible",
                             issued, sb.fetchConstant & 31);
        }
        // Fingerprint the INDEX INPUT as guest bytes, before any host endian
        // conversion. The oracle prints the same quantity. A matching vertex
        // count says only how much work was requested; this says whether the
        // primitives actually reference the same vertices.
        if (draw.indexed)
        {
            const uint64_t indexBytes = uint64_t(draw.indexCount) * (draw.indexIs32 ? 4u : 2u);
            if (uint64_t(draw.indexGuestBase) + indexBytes <= in.guestPhysicalMirrorBytes)
                lucent::info("draw",
                             "  draw {} geometry index base {:#x} bytes"
                             " {} format {} endian {} hash {:016x}",
                             issued, draw.indexGuestBase, indexBytes,
                             draw.indexIs32 ? "u32" : "u16", draw.indexEndian,
                             Fnv1a64(in.guestBase + draw.indexGuestBase, size_t(indexBytes)));
            else
                lucent::warn("draw",
                             "  draw {} geometry index range {:#x}+{}"
                             " exceeds the {}-byte mirror; NO index hash was computed",
                             issued, draw.indexGuestBase, indexBytes, in.guestPhysicalMirrorBytes);
        }
        else
            lucent::info("draw",
                         "  draw {} geometry index auto-sequential"
                         " count {} (no guest index bytes)",
                         issued, draw.indexCount);
        for (const auto &vb : vsX.vertexBindings)
        {
            const uint32_t fc = vb.fetchConstant & 95;
            const uint32_t d0 = R[0x4800 + fc * 2];
            if ((d0 & 3) != 3)
                continue;
            const uint32_t vbase = (d0 >> 2) << 2;
            const uint32_t stride = std::max(vb.strideWords, 1u);
            for (uint32_t v = 0; v < 4; ++v)
            {
                lucent::Line vl;
                vl.add("  draw {} vertex {} @ {:#x} (stride {} dwords):", issued, v,
                       vbase + v * stride * 4, stride);
                for (uint32_t w = 0; w < stride; ++w)
                {
                    const uint64_t off = uint64_t(vbase) + (v * stride + w) * 4;
                    if (off + 4 > in.guestPhysicalMirrorBytes)
                        break;
                    uint32_t raw;
                    std::memcpy(&raw, in.guestBase + off, 4);
                    raw = __builtin_bswap32(raw);
                    float f;
                    std::memcpy(&f, &raw, 4);
                    vl.add(" [{}]{:#010x}={}", w, raw, f);
                }
                vl.flush(lucent::Level::Info, "draw");
            }
            // AND THE WHOLE BUFFER, PER FIELD. Four vertices of nineteen
            // thousand is the cap-the-boring-case trap: a prefix that agrees
            // says nothing about whether the rest do, and catalog #91 spent a
            // session reasoning from four vertices about what all of them hold.
            // This walks every vertex the fetch constant covers and reports,
            // for each dword of the stride, how many DISTINCT values it takes
            // and the three commonest with their counts -- so "this field is
            // constant across the draw" and "the prefix happened to agree" stop
            // being the same reading.
            const uint32_t sizeDwords = (R[0x4800 + fc * 2 + 1] >> 2) & 0xFFFFFF;
            const uint64_t bufBytes = uint64_t(sizeDwords) * 4;
            if (uint64_t(vbase) + bufBytes <= in.guestPhysicalMirrorBytes)
                lucent::info("draw",
                             "  draw {} geometry fetch fc {} base"
                             " {:#x} bytes {} stride {} hash {:016x}",
                             issued, fc, vbase, bufBytes, stride,
                             Fnv1a64(in.guestBase + vbase, size_t(bufBytes)));
            else
                lucent::warn("draw",
                             "  draw {} geometry fetch fc {} range"
                             " {:#x}+{} exceeds the {}-byte mirror; NO fetch hash was"
                             " computed",
                             issued, fc, vbase, bufBytes, in.guestPhysicalMirrorBytes);
            uint64_t verts = stride ? bufBytes / (uint64_t(stride) * 4) : 0;
            constexpr uint64_t kScanCap = 200000;
            const bool capped = verts > kScanCap;
            if (capped)
                verts = kScanCap;
            for (uint32_t w = 0; w < stride && verts; ++w)
            {
                std::map<uint32_t, uint64_t> hist;
                for (uint64_t v2 = 0; v2 < verts; ++v2)
                {
                    const uint64_t off = uint64_t(vbase) + (v2 * stride + w) * 4;
                    if (off + 4 > in.guestPhysicalMirrorBytes)
                        break;
                    uint32_t raw;
                    std::memcpy(&raw, in.guestBase + off, 4);
                    ++hist[__builtin_bswap32(raw)];
                }
                if (hist.empty())
                    continue;
                std::vector<std::pair<uint32_t, uint64_t>> top(hist.begin(), hist.end());
                std::sort(top.begin(), top.end(),
                          [](const auto &a, const auto &b) { return a.second > b.second; });
                // AS FLOATS TOO. The min and max below are of the raw BITS,
                // which for a float field is not the value range at all --
                // every negative float outranks every positive one. The
                // position fields of a vertex layout are floats, and their
                // value range is the draw's bounding box, which is exactly
                // what a "why did this draw clip away" question wants.
                float fmin = std::numeric_limits<float>::infinity();
                float fmax = -std::numeric_limits<float>::infinity();
                bool allFinite = true;
                for (const auto &[bits, n] : hist)
                {
                    float f;
                    std::memcpy(&f, &bits, 4);
                    if (!std::isfinite(f))
                    {
                        allFinite = false;
                        continue;
                    }
                    fmin = std::min(fmin, f);
                    fmax = std::max(fmax, f);
                }
                lucent::Line hl;
                // MIN AND MAX, not just the commonest. For a field that is an
                // INDEX the tail is the whole question -- the commonest value
                // says nothing about whether some vertex reaches past the
                // matrices the draw uploaded -- and a top-N list hides exactly
                // that. hist is a std::map, so its ends are the extremes.
                hl.add("  draw {} field [{}] over {}{} vertices: {} distinct,"
                       " min {:#010x} max {:#010x};",
                       issued, w, capped ? ">= " : "", verts, hist.size(), hist.begin()->first,
                       hist.rbegin()->first);
                if (allFinite && fmin <= fmax)
                    hl.add(" as float {}..{};", fmin, fmax);
                else
                    hl.add(" (not all finite as float, so no float range);");
                for (size_t k = 0; k < top.size() && k < 3; ++k)
                    hl.add(" {:#010x}x{}", top[k].first, top[k].second);
                if (top.size() > 3)
                    hl.add(" (+{} more value(s) between those extremes, NOT shown)",
                           top.size() - 3);
                hl.flush(lucent::Level::Info, "draw");
            }
            if (verts == 0)
                lucent::warn("draw",
                             "  draw {}: the fetch constant covers {}"
                             " bytes at stride {} dwords, so ZERO whole vertices. No"
                             " distribution was computed -- that is a buffer this probe"
                             " could not read, not a field that is constant",
                             issued, bufBytes, stride);
        }
    }
}

void DumpVsConstants(const ShaderXlate &vsX, const UniformCache &uc, uint64_t vsHash,
                     uint32_t issued, uint32_t diagIndex, const uint32_t *R)
{
    // GEARS_DRAW_VS_CONSTS_VS=<16-hex vs hash>: EVERY draw of that shader, not
    // a diag index. The comparison this exists for is between two draws of the
    // SAME shader in the SAME frame -- one that clipped away and a neighbour
    // that did not -- and neither one's index is known before the frame runs.
    static const uint64_t byShader = []
    {
        const std::string &t = lucent::config::text("DRAW_VS_CONSTS_VS");
        if (t.empty())
            return uint64_t(0);
        char *end = nullptr;
        const uint64_t h = std::strtoull(t.c_str(), &end, 16);
        if (end == t.c_str() || h == 0)
        {
            lucent::warn("draw",
                         "GEARS_DRAW_VS_CONSTS_VS: cannot parse '{}' as"
                         " a 16-hex vertex shader hash; NOTHING is dumped by it",
                         t);
            return uint64_t(0);
        }
        lucent::info("draw",
                     "vertex constants armed for EVERY draw of shader"
                     " {:#018x}",
                     h);
        return h;
    }();
    const bool shaderMatch = byShader != 0 && vsHash == byShader;
    DrawSelection &sel = VsConstSelection();
    if (!shaderMatch && (!sel.Active() || !sel.Offer(diagIndex)))
        return;
    lucent::Line cl;
    // The constant-addressing mode is in the HEADER, not a footnote, because it
    // decides whether a fixed layout may be assumed of everything after it. A
    // shader that indexes its constants through a0 is doing a bone-palette
    // lookup, and its c0..c3 are bone rows, not a world matrix -- reading them
    // as one produces per-vertex verdicts that look exactly like real ones
    // (tools/clip_check.py did precisely that for a draw the GPU had
    // rasterised, and called every vertex of it BEHIND THE CAMERA).
    cl.add("draw {} (diag {}) vs {:#x} float constants ({} vec4s, in the"
           " shader's own packed order, addressing={}):",
           issued, diagIndex, vsHash, vsX.floatCount,
           vsX.floatDynamicAddressing ? "dynamic-skinned" : "static");
    // FLUSHED IN CHUNKS. A vertex shader can declare 256 float constants -- a
    // skinned mesh's bone palette is most of them -- and one Line of that is
    // ~22 KB, which the sink truncates. A truncated dump reads EXACTLY like a
    // complete one: this printed 47 of 256 vec4s for the skinned character in
    // catalog #77 with no indication, and 47 looks like a plausible constant
    // count, so it was read as "the shader only uses 47" and cost a detour.
    // Chunked, every constant is shown and each row says which range it covers.
    constexpr uint32_t kPerLine = 24;
    uint32_t shown = 0;
    for (uint32_t i = 0; i < vsX.floatCount && (i + 1) * 16 <= uc.fVs.size(); ++i)
    {
        float v[4];
        uint32_t b[4];
        std::memcpy(v, uc.fVs.data() + size_t(i) * 16, 16);
        std::memcpy(b, uc.fVs.data() + size_t(i) * 16, 16);
        cl.add(" c[{}]=({}, {}, {}, {})[{:08x} {:08x} {:08x} {:08x}]", i, v[0], v[1], v[2], v[3],
               b[0], b[1], b[2], b[3]);
        ++shown;
        if (shown % kPerLine == 0)
        {
            cl.flush(lucent::Level::Info, "draw");
            cl.add("draw {} (diag {}) vs {:#x} float constants, continued from"
                   " c[{}]:",
                   issued, diagIndex, vsHash, shown);
        }
    }
    cl.add("; {} of {} vec4s shown", shown, vsX.floatCount);
    cl.flush(lucent::Level::Info, "draw");
    // THE RECONCILIATION, and it is the point of this dump now. The bytes above
    // are what was PACKED for the draw's UBO; this compares them, slot by slot,
    // against the draw's OWN register-file snapshot -- the same draw, the same
    // quantity, two sources. A register watch and this dump gave contradictory
    // answers about c4 (issue #91) and no amount of reasoning about ordering
    // could settle it, because they were never made to measure the same thing.
    // The slot->register map is the same bitmap walk the packer uses, so the
    // two cannot drift; with dynamic addressing it is the identity.
    if (R != nullptr)
    {
        std::vector<uint32_t> slotReg;
        slotReg.reserve(vsX.floatCount);
        for (uint32_t block = 0; block < 4; ++block)
        {
            uint64_t entry = vsX.floatBitmap[block];
            while (entry)
            {
                const uint32_t idx = uint32_t(std::countr_zero(entry));
                entry &= ~(uint64_t(1) << idx);
                slotReg.push_back(block * 64 + idx);
            }
        }
        uint32_t compared = 0, mismatched = 0;
        lucent::Line ml;
        for (uint32_t i = 0; i < shown && i < slotReg.size(); ++i)
        {
            uint32_t b[4];
            std::memcpy(b, uc.fVs.data() + size_t(i) * 16, 16);
            for (uint32_t c = 0; c < 4; ++c)
            {
                const uint32_t regv = R[0x4000 + slotReg[i] * 4 + c];
                ++compared;
                if (regv == b[c])
                    continue;
                ++mismatched;
                if (mismatched <= 6)
                    ml.add(" c[{}](guest c{}).{}: UBO {:08x} vs register file"
                           " {:08x};",
                           i, slotReg[i], "xyzw"[c], b[c], regv);
            }
        }
        lucent::Line hd;
        hd.add("draw {} (diag {}) UBO-vs-REGISTER-FILE: {} of {} components"
               " DIFFER{}",
               issued, diagIndex, mismatched, compared,
               mismatched == 0 ? " -- the packed constants ARE the draw's register snapshot"
                               : ", so the draw did NOT read what the guest last wrote:");
        hd.flush(lucent::Level::Info, "draw");
        if (!ml.empty())
        {
            if (mismatched > 6)
                ml.add(" (+{} more, NOT shown)", mismatched - 6);
            ml.flush(lucent::Level::Info, "draw");
        }
    }
    // The constants the shader DECLARES versus the bytes actually packed for
    // it. If the block is short, the tail the shader reads is not in this dump
    // and a "the transforms are identical" conclusion drawn from it would be
    // reading a prefix.
    if (uc.fVs.size() < size_t(vsX.floatCount) * 16)
        cl.add("; ONLY {} of {} vec4s were packed -- the rest are not shown", uc.fVs.size() / 16,
               vsX.floatCount);
    cl.flush(lucent::Level::Info, "draw");
}

void ListDraw(const uint32_t *R, const FrameDrawItem &d, const FrameDrawInputs &in,
              const ShaderXlate &vsX, const ShaderXlate &psX, const UniformCache &uc,
              uint32_t issued)
{
    lucent::Line dl;
    dl.add("  draw {}: {} {} {} verts, vs {:#018x} ({} tex) ps {:#018x} ({} tex),"
           " colormask {:#x} blend {:#x} depth {:#x}",
           issued, PrimName(d.primType), d.indexed ? "indexed" : "auto", d.indexCount, d.vsHash,
           vsX.textures.size(), d.psHash, psX.textures.size(), R[0x2104] /*RB_COLOR_MASK*/,
           R[0x2201] /*RB_BLENDCONTROL0*/, R[0x2200] /*RB_DEPTHCONTROL*/);
    for (const auto &t : psX.textures)
        dl.add(" tex[fc{}]={:#x}", t.fetchConstant,
               (R[0x4800 + (t.fetchConstant & 31) * 6 + 1] >> 12) << 12);
    // Where this draw's GEOMETRY comes from, and whether the SSBO
    // mirror actually covers it. A vfetch past the mirror reads zero,
    // which collapses every triangle -- indistinguishable in the output
    // from "shaded black", so it has to be reported explicitly.
    for (const auto &vb : vsX.vertexBindings)
    {
        const uint32_t fc = vb.fetchConstant & 95;
        const uint32_t d0 = R[0x4800 + fc * 2];
        const uint32_t d1 = R[0x4800 + fc * 2 + 1];
        const uint32_t vbase = (d0 >> 2) << 2;              // dword address -> bytes
        const uint32_t vbytes = ((d1 >> 2) & 0xFFFFFF) * 4; // size in dwords -> bytes
        dl.add(" vf[fc{}]type{}={:#x}+{:#x}{}", fc, d0 & 3, vbase, vbytes,
               uint64_t(vbase) + vbytes <= in.guestPhysicalMirrorBytes ? "" : " PAST-MIRROR");
    }
    // The float constants the shaders actually received. Captured output shows
    // that this scalar controls final colour intensity for most draws in the
    // frame, so printing it separates valid black output from missing inputs.
    auto nonZero = [](const std::vector<uint8_t> &v)
    {
        size_t n = 0;
        for (size_t i = 0; i + 4 <= v.size(); i += 4)
            if (v[i] || v[i + 1] || v[i + 2] || v[i + 3])
                ++n;
        return n;
    };
    float psC255 = 0.0f;
    const uint32_t c255bits = R[0x4400 + 255 * 4];
    std::memcpy(&psC255, &c255bits, 4);
    dl.add(" vsconst {}/{} nz, psconst {}/{} nz, ps c255.x={} ({:#x})", nonZero(uc.fVs),
           vsX.floatCount, nonZero(uc.fPs), psX.floatCount, psC255, c255bits);
    // GEARS_DRAW_PS_CONSTS=<hash> prints the pixel float constants a named
    // shader actually received, as the numbers the shader will multiply by.
    // "psconst 9/3 nz" says three of nine are non-zero and cannot say WHICH,
    // and for a pass that ends in a scale the difference between "the scale
    // is 0.9" and "the scale is 0" is the difference between a frame and a
    // black screen.
    if (const std::string &want = lucent::config::text("DRAW_PS_CONSTS"); !want.empty())
    {
        const uint64_t wantHash = std::strtoull(want.c_str(), nullptr, 16);
        if (wantHash == d.psHash)
        {
            lucent::Line cl;
            cl.add("draw {} ps {:#x} float constants ({} vec4s, in the"
                   " shader's own packed order):",
                   issued, d.psHash, psX.floatCount);
            // THE GUEST REGISTER EACH PACKED SLOT CAME FROM. The packed index
            // is a position in the shader's constant map and nothing can be
            // done with it: "c[14].w is wrong" names no register to go and
            // look at, and the two emulators' dumps agree on the packing so
            // the index alone cannot even be cross-checked. Recomputed here by
            // the same bitmap walk PackFloatConstants uses, so the two cannot
            // drift apart.
            std::vector<uint32_t> slotReg;
            slotReg.reserve(psX.floatCount);
            for (uint32_t block = 0; block < 4; ++block)
            {
                uint64_t entry = psX.floatBitmap[block];
                while (entry)
                {
                    const uint32_t idx = uint32_t(std::countr_zero(entry));
                    entry &= ~(uint64_t(1) << idx);
                    slotReg.push_back(block * 64 + idx);
                }
            }
            for (uint32_t i = 0; i < psX.floatCount && (i + 1) * 16 <= uc.fPs.size(); ++i)
            {
                float v[4];
                uint32_t b[4];
                std::memcpy(v, uc.fPs.data() + size_t(i) * 16, 16);
                std::memcpy(b, uc.fPs.data() + size_t(i) * 16, 16);
                // The RAW BITS as well, because a NaN or an inf printed as
                // a word says nothing about where it came from: 0xffffffff
                // is uninitialised memory, 0x7fc00000 is arithmetic, and the
                // two point at completely different bugs.
                cl.add(" c[{}](guest c{})=({}, {}, {}, {})"
                       "[{:08x} {:08x} {:08x} {:08x}]",
                       i, i < slotReg.size() ? int32_t(slotReg[i]) : -1, v[0], v[1], v[2], v[3],
                       b[0], b[1], b[2], b[3]);
            }
            cl.flush(lucent::Level::Info, "draw");
        }
    }
    dl.flush(lucent::Level::Info, "draw");
}

} // namespace gears::draw
