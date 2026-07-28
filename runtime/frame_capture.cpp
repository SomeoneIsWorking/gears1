// See frame_capture.h for why this exists.
#include "frame_capture.h"

#include <lucent/log.h>

#include <cstring>
#include <cstdio>
#include <map>
#include <string>

namespace gears
{
namespace
{

constexpr char kMagic[8] = {'G', 'E', 'A', 'R', 'S', 'F', 'R', '1'};
constexpr uint32_t kVersion = 1;
// Guest memory is stored per block so an untouched region costs nothing. 64 KiB
// is small enough that a texture or vertex buffer does not drag in megabytes of
// neighbouring emptiness, and large enough that the index stays short.
constexpr uint32_t kBlockSize = 0x10000;

// A tiny checked writer/reader pair. Every field goes through these, so a
// truncated file fails at the field that is missing instead of being read as
// whatever follows it.
struct Writer
{
    std::FILE* f = nullptr;
    bool ok = true;
    void raw(const void* p, size_t n)
    {
        if (!ok) return;
        if (std::fwrite(p, 1, n, f) != n) ok = false;
    }
    template <typename T> void pod(const T& v) { raw(&v, sizeof(T)); }
};

struct Reader
{
    std::FILE* f = nullptr;
    bool ok = true;
    void raw(void* p, size_t n)
    {
        if (!ok) return;
        if (std::fread(p, 1, n, f) != n) ok = false;
    }
    template <typename T> T pod()
    {
        T v{};
        raw(&v, sizeof(T));
        return v;
    }
};

bool BlockIsZero(const uint8_t* p, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
        if (p[i]) return false;
    return true;
}

} // namespace

bool WriteFrameCapture(const std::filesystem::path& path, const FrameDrawInputs& in)
{
    if (!in.guestBase)
    {
        lucent::error("draw", "frame capture: no guest memory to capture");
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    Writer w;
    w.f = std::fopen(path.string().c_str(), "wb");
    if (!w.f)
    {
        lucent::error("draw", "frame capture: cannot open {} for writing", path.string());
        return false;
    }

    // Microcode blobs, deduplicated by hash. A frame binds a few dozen distinct
    // shaders across hundreds of draws, so this is the difference between a few
    // hundred KiB and several MiB -- and, more usefully, it makes the blob table
    // a census of the frame's distinct shaders.
    std::map<uint64_t, uint32_t> blobIndex; // hash -> index
    struct Blob { uint64_t hash; const uint8_t* data; uint32_t size; };
    std::vector<Blob> blobs;
    auto intern = [&](const uint8_t* data, size_t size, uint64_t hash) -> uint32_t {
        if (!data || size == 0) return UINT32_MAX;
        auto it = blobIndex.find(hash);
        if (it != blobIndex.end()) return it->second;
        const uint32_t idx = uint32_t(blobs.size());
        blobs.push_back({hash, data, uint32_t(size)});
        blobIndex[hash] = idx;
        return idx;
    };
    struct DrawRef { uint32_t vs, ps; };
    std::vector<DrawRef> refs;
    refs.reserve(in.draws.size());
    for (const FrameDrawItem& d : in.draws)
        refs.push_back({intern(d.vsUcode, d.vsUcodeSize, d.vsHash),
                        intern(d.psUcode, d.psUcodeSize, d.psHash)});

    // Non-zero guest blocks. The scan covers the whole window the renderer is
    // allowed to read, because a texture fetch constant can name any of it.
    const uint32_t window = in.guestWindowBytes;
    std::vector<uint32_t> blockIds;
    for (uint32_t off = 0; off < window; off += kBlockSize)
    {
        const uint32_t n = std::min<uint32_t>(kBlockSize, window - off);
        if (!BlockIsZero(in.guestBase + off, n))
            blockIds.push_back(off / kBlockSize);
    }

    w.raw(kMagic, sizeof(kMagic));
    w.pod(kVersion);
    w.pod(in.width);
    w.pod(in.height);
    w.pod(in.guestPhysicalMirrorBytes);
    w.pod(window);
    w.pod(int64_t(in.sequence));
    w.pod(uint32_t(in.draws.size()));
    w.pod(kBlockSize);
    w.pod(uint32_t(blockIds.size()));
    for (uint32_t b : blockIds)
        w.pod(b);
    for (uint32_t b : blockIds)
    {
        const uint32_t off = b * kBlockSize;
        w.raw(in.guestBase + off, std::min<uint32_t>(kBlockSize, window - off));
    }
    w.pod(uint32_t(blobs.size()));
    for (const Blob& b : blobs)
    {
        w.pod(b.hash);
        w.pod(b.size);
        w.raw(b.data, b.size);
    }
    for (size_t i = 0; i < in.draws.size(); ++i)
    {
        const FrameDrawItem& d = in.draws[i];
        w.pod(uint32_t(d.registerFile.size()));
        if (!d.registerFile.empty())
            w.raw(d.registerFile.data(), d.registerFile.size() * sizeof(uint32_t));
        w.pod(refs[i].vs);
        w.pod(refs[i].ps);
        w.pod(d.primType);
        w.pod(d.indexCount);
        w.pod(uint32_t((d.indexed ? 1u : 0u) | (d.indexIs32 ? 2u : 0u)));
        w.pod(d.indexGuestBase);
    }
    const bool ok = w.ok;
    std::fclose(w.f);
    if (!ok)
    {
        // A half-written capture that replays is worse than none: it would render
        // a frame that never existed and the difference would be invisible.
        std::filesystem::remove(path, ec);
        lucent::error("draw", "frame capture: write to {} failed, capture removed",
                      path.string());
        return false;
    }
    const uint64_t bytes = uint64_t(blockIds.size()) * kBlockSize;
    lucent::info("draw", "frame capture: wrote {} ({} draws, {} distinct shaders,"
        " {} MiB of guest memory in {} blocks)", path.string(), in.draws.size(),
        blobs.size(), bytes / (1024 * 1024), blockIds.size());
    return true;
}

bool ReadFrameCapture(const std::filesystem::path& path, FrameCapture& out)
{
    Reader r;
    r.f = std::fopen(path.string().c_str(), "rb");
    if (!r.f)
    {
        lucent::error("draw", "frame capture: cannot open {}", path.string());
        return false;
    }
    char magic[8]{};
    r.raw(magic, sizeof(magic));
    if (!r.ok || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0)
    {
        std::fclose(r.f);
        lucent::error("draw", "frame capture: {} is not a frame capture", path.string());
        return false;
    }
    const uint32_t version = r.pod<uint32_t>();
    if (version != kVersion)
    {
        std::fclose(r.f);
        lucent::error("draw", "frame capture: {} is version {}, this build reads {}",
                      path.string(), version, kVersion);
        return false;
    }
    out.inputs.width = r.pod<uint32_t>();
    out.inputs.height = r.pod<uint32_t>();
    out.inputs.guestPhysicalMirrorBytes = r.pod<uint32_t>();
    const uint32_t window = r.pod<uint32_t>();
    out.inputs.guestWindowBytes = window;
    out.inputs.sequence = long(r.pod<int64_t>());
    const uint32_t drawCount = r.pod<uint32_t>();
    const uint32_t blockSize = r.pod<uint32_t>();
    const uint32_t blockCount = r.pod<uint32_t>();
    if (!r.ok || blockSize == 0)
    {
        std::fclose(r.f);
        lucent::error("draw", "frame capture: {} header is truncated", path.string());
        return false;
    }
    std::vector<uint32_t> blockIds(blockCount);
    for (uint32_t i = 0; i < blockCount; ++i)
        blockIds[i] = r.pod<uint32_t>();
    // The window is reconstructed in full and zero-filled: a block that was not
    // captured was zero when the frame was captured, so reading it back as zero
    // is exact, not a stand-in.
    out.guest.assign(window, 0u);
    for (uint32_t b : blockIds)
    {
        const uint64_t off = uint64_t(b) * blockSize;
        if (off >= window) { r.ok = false; break; }
        r.raw(out.guest.data() + off,
              std::min<uint64_t>(blockSize, uint64_t(window) - off));
    }
    const uint32_t blobCount = r.ok ? r.pod<uint32_t>() : 0;
    out.ucode.assign(blobCount, {});
    std::vector<uint64_t> blobHash(blobCount, 0);
    for (uint32_t i = 0; i < blobCount && r.ok; ++i)
    {
        blobHash[i] = r.pod<uint64_t>();
        const uint32_t size = r.pod<uint32_t>();
        if (!r.ok) break;
        out.ucode[i].resize(size);
        r.raw(out.ucode[i].data(), size);
    }
    out.inputs.draws.clear();
    out.inputs.draws.reserve(drawCount);
    for (uint32_t i = 0; i < drawCount && r.ok; ++i)
    {
        FrameDrawItem d;
        const uint32_t regDwords = r.pod<uint32_t>();
        if (!r.ok) break;
        d.registerFile.resize(regDwords);
        if (regDwords)
            r.raw(d.registerFile.data(), size_t(regDwords) * sizeof(uint32_t));
        const uint32_t vs = r.pod<uint32_t>();
        const uint32_t ps = r.pod<uint32_t>();
        d.primType = r.pod<uint32_t>();
        d.indexCount = r.pod<uint32_t>();
        const uint32_t flags = r.pod<uint32_t>();
        d.indexGuestBase = r.pod<uint32_t>();
        d.indexed = (flags & 1) != 0;
        d.indexIs32 = (flags & 2) != 0;
        if (vs < blobCount)
        {
            d.vsUcode = out.ucode[vs].data();
            d.vsUcodeSize = out.ucode[vs].size();
            d.vsHash = blobHash[vs];
        }
        if (ps < blobCount)
        {
            d.psUcode = out.ucode[ps].data();
            d.psUcodeSize = out.ucode[ps].size();
            d.psHash = blobHash[ps];
        }
        out.inputs.draws.push_back(std::move(d));
    }
    const bool ok = r.ok && out.inputs.draws.size() == drawCount;
    std::fclose(r.f);
    if (!ok)
    {
        lucent::error("draw", "frame capture: {} is truncated ({} of {} draws read)",
                      path.string(), out.inputs.draws.size(), drawCount);
        return false;
    }
    out.inputs.guestBase = out.guest.data();
    lucent::info("draw", "frame capture: loaded {} ({} draws, {} distinct shaders,"
        " frame {})", path.string(), drawCount, blobCount, out.inputs.sequence);
    return true;
}

} // namespace gears
