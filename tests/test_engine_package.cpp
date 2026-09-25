// Synthetic contracts for the native engine's package layer: the LZO1X
// decoder, the three on-disc package forms, and refusal of malformed input.

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "package/content_files.h"
#include "package/lzo1x.h"
#include "package/package.h"
#include "package/package_constants.h"

namespace
{

using gears::engine::package::DecodeLzo1x;
using gears::engine::package::DecompressionError;
using gears::engine::package::Package;
using gears::engine::package::PackageFormatError;
using Bytes = std::vector<std::uint8_t>;

template <typename Error, typename Action> bool Refuses(Action action)
{
    try
    {
        action();
    }
    catch (const Error &)
    {
        return true;
    }
    return false;
}

Bytes Decode(const Bytes &stream, std::size_t size)
{
    Bytes output(size);
    DecodeLzo1x(stream, output);
    return output;
}

void TestLzo1x()
{
    // Four leading literals, a 4-byte short match at distance 4, end marker.
    Bytes repeat{0x15, 'a', 'b', 'c', 'd', 0x6C, 0x00, 0x11, 0x00, 0x00};
    assert((Decode(repeat, 8) == Bytes{'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd'}));

    // An extended literal run (3 + 15 + 5 = 23 bytes), then a medium match of
    // five bytes at distance one, which overlaps its own output.
    Bytes run{0x00, 0x05};
    for (std::uint8_t i = 0; i < 23U; ++i)
    {
        run.push_back(static_cast<std::uint8_t>('A' + i));
    }
    run.insert(run.end(), {0x23, 0x00, 0x00, 0x11, 0x00, 0x00});
    Bytes decoded = Decode(run, 28);
    assert(decoded[22] == 'W');
    for (std::size_t i = 23; i < 28; ++i)
    {
        assert(decoded[i] == 'W');
    }

    // A far match carries trailing literals in its distance word's low bits:
    // an extended run of 20002 literals (3 + 15 + 78 * 255 + 94) starting
    // "abcd", a four-byte match at distance 20002 (16384 + 3618) with two
    // trailing literals, and the end marker.
    Bytes far{0x00};
    far.insert(far.end(), 78U, 0x00);
    far.push_back(94);
    far.insert(far.end(), {'a', 'b', 'c', 'd'});
    far.insert(far.end(), 20002U - 4U, 'x');
    far.insert(far.end(), {0x12, 0x8A, 0x38, 'p', 'q', 0x11, 0x00, 0x00});
    Bytes far_decoded = Decode(far, 20008);
    assert((Bytes(far_decoded.begin() + 20002, far_decoded.end()) ==
            Bytes{'a', 'b', 'c', 'd', 'p', 'q'}));

    Bytes truncated(repeat.begin(), repeat.end() - 3);
    assert(Refuses<DecompressionError>([&] { Decode(truncated, 8); }));
    assert(Refuses<DecompressionError>([&] { Decode(repeat, 7); }));
    assert(Refuses<DecompressionError>([&] { Decode(repeat, 9); }));
    Bytes too_far{0x15, 'a', 'b', 'c', 'd', 0x6C, 0x01, 0x11, 0x00, 0x00};
    assert(Refuses<DecompressionError>([&] { Decode(too_far, 8); }));
}

class Writer
{
  public:
    void U32(std::uint32_t value)
    {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            bytes.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
        }
    }
    void U32Little(std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32U; shift += 8U)
        {
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    }
    void String(std::string_view text)
    {
        U32(static_cast<std::uint32_t>(text.size() + 1U));
        bytes.insert(bytes.end(), text.begin(), text.end());
        bytes.push_back(0);
    }
    void Name(std::uint32_t index, std::uint32_t number = 0)
    {
        U32(index);
        U32(number);
    }
    void Zeros(std::size_t count) { bytes.insert(bytes.end(), count, 0U); }
    void Patch(std::size_t offset, std::uint32_t value)
    {
        for (std::size_t i = 0; i < 4U; ++i)
        {
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (24U - 8U * i));
        }
    }

    Bytes bytes;
};

constexpr std::size_t kSummaryNamesField = 0x19;
constexpr std::size_t kSummaryExportsField = 0x21;
constexpr std::size_t kSummaryImportsField = 0x29;
constexpr std::size_t kSummaryHeaderSizeField = 0x08;
const Bytes kExportPayload{0xDE, 0xAD, 0xBE, 0xEF};

// The uncompressed summary ends here; a chunked file's table starts here.
constexpr std::size_t kChunkTableField = 0x61;

// An uncompressed package: names Core, Package, Thing; import 0 is the Core
// package; export "Thing_2" is an instance of that import with a four-byte
// payload.
Bytes PlainPackage(std::uint32_t version = gears::engine::package::kGears1PackageFileVersion,
                   std::uint32_t export_name = 2)
{
    Writer w;
    w.U32(gears::engine::package::kPackageTag);
    w.U32(version);
    w.U32(0); // header size, patched
    w.String("None");
    w.U32(0x00080009U);
    w.Zeros(24); // name/export/import locations, patched
    w.Zeros(16); // guid
    w.U32(1);
    w.U32(1);
    w.U32(3);
    w.U32(0);
    w.U32(2451);
    w.U32(32);
    w.U32(0); // no compression
    w.U32(0); // no chunks
    w.Patch(kSummaryNamesField, 3);
    w.Patch(kSummaryNamesField + 4, static_cast<std::uint32_t>(w.bytes.size()));
    for (const char *name : {"Core", "Package", "Thing"})
    {
        w.String(name);
        w.U32(0x00070010U);
        w.U32(0);
    }
    w.Patch(kSummaryImportsField, 1);
    w.Patch(kSummaryImportsField + 4, static_cast<std::uint32_t>(w.bytes.size()));
    w.Name(0);
    w.Name(1);
    w.U32(0);
    w.Name(0);
    w.Patch(kSummaryExportsField, 1);
    w.Patch(kSummaryExportsField + 4, static_cast<std::uint32_t>(w.bytes.size()));
    w.U32(0xFFFFFFFFU); // class: import 0
    w.U32(0);
    w.U32(0);
    w.Name(export_name, 3);
    w.U32(0);
    w.U32(0x000F0004U);
    w.U32(0);
    w.U32(static_cast<std::uint32_t>(kExportPayload.size()));
    std::size_t serial_offset_field = w.bytes.size();
    w.U32(0);
    w.U32(0);
    w.U32(0);
    w.U32(0);
    w.Zeros(16);
    auto header_size = static_cast<std::uint32_t>(w.bytes.size());
    w.Patch(kSummaryHeaderSizeField, header_size);
    w.Patch(serial_offset_field, header_size);
    w.bytes.insert(w.bytes.end(), kExportPayload.begin(), kExportPayload.end());
    return w.bytes;
}

// One literal-only LZO1X block of at least four bytes: a literal-run
// instruction (a first byte below 18 is an ordinary instruction) with its
// extended length, the data, and the end marker.
Bytes LiteralBlock(std::span<const std::uint8_t> data)
{
    assert(data.size() >= 4U);
    Bytes block;
    std::size_t run = data.size() - 3U;
    if (run <= 15U)
    {
        block.push_back(static_cast<std::uint8_t>(run));
    }
    else
    {
        block.push_back(0x00);
        std::size_t extension = run - 15U;
        while (extension > 255U)
        {
            block.push_back(0x00);
            extension -= 255U;
        }
        block.push_back(static_cast<std::uint8_t>(extension));
    }
    block.insert(block.end(), data.begin(), data.end());
    block.insert(block.end(), {0x11, 0x00, 0x00});
    return block;
}

Bytes Record(std::span<const std::uint8_t> data, bool little_endian)
{
    Writer w;
    Bytes block = LiteralBlock(data);
    auto put = [&](std::uint32_t value) { little_endian ? w.U32Little(value) : w.U32(value); };
    put(gears::engine::package::kPackageTag);
    put(gears::engine::package::kPackageTag);
    put(static_cast<std::uint32_t>(block.size()));
    put(static_cast<std::uint32_t>(data.size()));
    put(static_cast<std::uint32_t>(block.size()));
    put(static_cast<std::uint32_t>(data.size()));
    w.bytes.insert(w.bytes.end(), block.begin(), block.end());
    return w.bytes;
}

void CheckThing(const Package &package)
{
    assert(package.Tables().names.size() == 3U);
    assert(package.ObjectPath(1) == "Thing_2");
    assert(package.ObjectPath(-1) == "Core");
    assert(package.FullPath(1) == "Test.Thing_2");
    assert(package.FullPath(-1) == "Core");
    assert(package.OutermostName(1) == "Thing_2");
    assert(package.ClassName(1) == "Core");
    assert(package.ClassName(-1) == "Package");
    std::span<const std::uint8_t> data = package.ExportData(0);
    assert((Bytes(data.begin(), data.end()) == kExportPayload));
}

void TestPlainPackage()
{
    CheckThing(Package::Load("Test", PlainPackage()));
    assert(Refuses<PackageFormatError>([] { (void)Package::Load("Test", PlainPackage(375)); }));
    assert(Refuses<PackageFormatError>([] { (void)Package::Load("Test", PlainPackage(374, 9)); }));
    Bytes padded = PlainPackage();
    padded.resize(padded.size() + 64U, 0U);
    assert(Package::Load("Test", padded).Bytes().size() == PlainPackage().size());
    padded.back() = 1U;
    assert(Refuses<PackageFormatError>([&] { (void)Package::Load("Test", padded); }));
    Bytes truncated = PlainPackage();
    truncated.resize(truncated.size() - 1U);
    assert(Refuses<PackageFormatError>([&] { (void)Package::Load("Test", truncated); }));
}

void TestWholeFileCompressedPackage()
{
    Bytes file = Record(PlainPackage(), true);
    file.resize(file.size() + 32U, 0U);
    CheckThing(Package::Load("Test", file));
    file.back() = 1U;
    assert(Refuses<PackageFormatError>([&] { (void)Package::Load("Test", file); }));
}

// The file keeps the plain summary, adds the LZO method and one chunk entry,
// and holds everything after the uncompressed summary as one big-endian
// record. Chunk offsets address the package without the chunk table.
Bytes ChunkedPackage()
{
    Bytes plain = PlainPackage();
    Bytes rest(plain.begin() + static_cast<std::ptrdiff_t>(kChunkTableField), plain.end());
    Bytes record = Record(rest, false);
    Writer w;
    w.bytes.assign(plain.begin(),
                   plain.begin() + static_cast<std::ptrdiff_t>(kChunkTableField) - 8);
    w.U32(2);
    w.U32(1);
    w.U32(static_cast<std::uint32_t>(kChunkTableField));
    w.U32(static_cast<std::uint32_t>(rest.size()));
    w.U32(static_cast<std::uint32_t>(kChunkTableField + 16U));
    w.U32(static_cast<std::uint32_t>(record.size()));
    w.bytes.insert(w.bytes.end(), record.begin(), record.end());
    return w.bytes;
}

// The summary stays plain; everything after it is one big-endian record.
void TestChunkCompressedPackage()
{
    Bytes file = ChunkedPackage();
    CheckThing(Package::Load("Test", file));

    Writer gap;
    gap.bytes = file;
    gap.Patch(kChunkTableField, static_cast<std::uint32_t>(kChunkTableField + 1U));
    assert(Refuses<PackageFormatError>([&] { (void)Package::Load("Test", gap.bytes); }));
    Writer outside;
    outside.bytes = file;
    outside.Patch(kChunkTableField + 12U, static_cast<std::uint32_t>(file.size()));
    assert(Refuses<PackageFormatError>([&] { (void)Package::Load("Test", outside.bytes); }));
}

} // namespace

// The file reader returns every byte of a file and refuses one it cannot open.
void TestReadPackageFile()
{
    std::filesystem::path path = "test_engine_package_file.xxx";
    Bytes written{0xC1, 0x83, 0x2A, 0x9E, 0x00, 0x01, 0x02};
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(written.data()),
                     static_cast<std::streamsize>(written.size()));
    }
    assert(gears::engine::package::ReadPackageFile(path) == written);
    std::filesystem::remove(path);
    assert(
        Refuses<PackageFormatError>([&] { (void)gears::engine::package::ReadPackageFile(path); }));
}

int main()
{
    TestReadPackageFile();
    TestLzo1x();
    TestPlainPackage();
    TestWholeFileCompressedPackage();
    TestChunkCompressedPackage();
    return 0;
}
