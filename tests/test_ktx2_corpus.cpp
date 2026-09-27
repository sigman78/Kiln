// tests/test_ktx2_corpus.cpp — real-world KTX2 corpus (tests/corpus/ktx2); reader-only, shipping runs it.
// Khronos KTX-Software files plus `ktx create` variants; manifest.txt holds `ktx info` ground truth.
// Runs only when kiln_tests gets `--corpus <dir>`.
#include "kiln_test.h"
#include "ktx2_corpus.h"

#include "kiln/containers.h"
#include "kiln/ktx2.h"

using namespace kiln;
using namespace kiln::ktx2;
namespace corpus = kiln::test::corpus;

namespace {

/// Checks for a file the manifest expects to open.
void check_ok_file(corpus::Entry const& e, Span<u8 const> bytes) {
    char name[256];
    format(name, sizeof name, "%.*s", KILN_SV(e.path));

    corpus::DiagCapture cap;
    DiagSink sink      = cap.sink();
    Result<Ktx2View> r = Ktx2View::open(bytes, &sink, e.path);
    if (!KILN_CHECK_MSG(r.ok(), "%s: open failed: %s", name, cap.msg)) return;
    KILN_CHECK_MSG(cap.count == 0, "%s: %d unexpected diagnostic(s), first: %s", name, cap.count, cap.msg);

    Ktx2View const& v   = r.value();
    TextureDesc const d = v.desc();
    KILN_CHECK_MSG(d.format == e.format, "%s: format %s, manifest %.*s", name, format_name(d.format),
                   KILN_SV(e.formatName));
    KILN_CHECK_MSG(d.width == e.width && d.height == e.height, "%s: %ux%u, manifest %ux%u", name, d.width,
                   d.height, e.width, e.height);
    KILN_CHECK_MSG(d.depth == max(e.depth, 1u), "%s: depth %u, manifest pixelDepth %u", name, d.depth,
                   e.depth);
    KILN_CHECK_MSG(d.layers == max(e.layers, 1u) && d.isArray == (e.layers != 0),
                   "%s: layers %u isArray %d, manifest layerCount %u", name, d.layers, int(d.isArray),
                   e.layers);
    KILN_CHECK_MSG(d.faces == e.faces && d.isCube == (e.faces == 6), "%s: faces %u isCube %d, manifest %u",
                   name, d.faces, int(d.isCube), e.faces);
    KILN_CHECK_MSG(d.levels == max(e.levels, 1u), "%s: levels %u, manifest levelCount %u", name, d.levels,
                   e.levels);
    KILN_CHECK_MSG(v.header().levelCount == e.levels, "%s: header levelCount %u, manifest %u", name,
                   v.header().levelCount, e.levels);
    KILN_CHECK_MSG(v.header().supercompressionScheme == e.supercompression,
                   "%s: supercompressionScheme %u, manifest %u", name, v.header().supercompressionScheme,
                   e.supercompression);
    KILN_CHECK_MSG(v.has_all_level_data(), "%s: level data missing from a whole-file open", name);

    // Level sizes (the kiln-info --check rule) and smallest-first storage.
    Span<LevelIndex const> const li = v.levels();
    KILN_CHECK_EQ(li.size, usize(d.levels));
    u64 const images = u64(d.faces) * d.layers;
    for (u32 i = 0; i < li.size; ++i) {
        u64 const want = v.level_image_bytes(i) * images;
        KILN_CHECK_MSG(want != 0 && li[i].byteLength == want, "%s: level %u byteLength %llu, expected %llu",
                       name, i, static_cast<unsigned long long>(li[i].byteLength),
                       static_cast<unsigned long long>(want));
        KILN_CHECK_MSG(v.level_data(i).size == li[i].byteLength, "%s: level %u data span %zu bytes", name, i,
                       v.level_data(i).size);
        if (i > 0)
            KILN_CHECK_MSG(li[i].byteOffset + li[i].byteLength <= li[i - 1].byteOffset,
                           "%s: level %u (offset %llu) not stored before level %u (offset %llu)", name, i,
                           static_cast<unsigned long long>(li[i].byteOffset), i - 1,
                           static_cast<unsigned long long>(li[i - 1].byteOffset));
    }

    // Expected key/value entries.
    corpus::for_each_item(e.keys, [&](StrView key) {
        KILN_CHECK_MSG(!v.find_key(key).empty(), "%s: KVD key %.*s not found", name, KILN_SV(key));
    });

    // Prefix open: only the metadata (header, level index, DFD, KVD).
    u64 const meta = Ktx2View::metadata_size(v.header());
    if (!KILN_CHECK_MSG(meta <= bytes.size, "%s: metadata_size %llu > file size %zu", name,
                        static_cast<unsigned long long>(meta), bytes.size))
        return;
    corpus::DiagCapture pcap;
    DiagSink psink      = pcap.sink();
    Result<Ktx2View> pr = Ktx2View::open(bytes.first(usize(meta)), &psink, e.path);
    if (KILN_CHECK_MSG(pr.ok(), "%s: prefix open (%llu bytes) failed: %s", name,
                       static_cast<unsigned long long>(meta), pcap.msg)) {
        KILN_CHECK_MSG(pr->level_data(0).empty(), "%s: prefix open exposes level 0 data", name);
        KILN_CHECK_MSG(!pr->has_all_level_data(), "%s: prefix open claims all level data", name);
        KILN_CHECK_MSG(pr->desc().width == d.width && pr->desc().levels == d.levels,
                       "%s: prefix desc differs", name);
        KILN_CHECK_MSG(corpus::bytes_equal(pr->dfd(), v.dfd()) && corpus::bytes_equal(pr->kvd(), v.kvd()),
                       "%s: prefix DFD/KVD differ from whole-file open", name);
    }

    // Prefix up to the first level byte (metadata plus any padding before the data).
    u64 firstLevel = li[0].byteOffset;
    for (u32 i = 1; i < li.size; ++i)
        firstLevel = min(firstLevel, li[i].byteOffset);
    Result<Ktx2View> fr = Ktx2View::open(bytes.first(usize(firstLevel)), nullptr, e.path);
    if (KILN_CHECK_MSG(fr.ok(), "%s: open of the first %llu bytes (up to the first level) failed", name,
                       static_cast<unsigned long long>(firstLevel))) {
        for (u32 i = 0; i < fr->desc().levels; ++i)
            KILN_CHECK_MSG(fr->level_data(i).empty(), "%s: level %u data present before its offset", name, i);
    }
}

/// Checks for a file the manifest expects to be rejected as unsupported.
void check_unsupported_file(corpus::Entry const& e, Span<u8 const> bytes) {
    corpus::DiagCapture cap;
    DiagSink sink      = cap.sink();
    Result<Ktx2View> r = Ktx2View::open(bytes, &sink, e.path);
    KILN_CHECK_MSG(!r.ok() && r.code() == Code::Unsupported, "%.*s: expected Unsupported, got status %u (%s)",
                   KILN_SV(e.path), u32(r.code()), cap.msg);
    KILN_CHECK_MSG(cap.code == e.code && cap.errors == 1,
                   "%.*s: diagnostic K%u (%d errors), manifest K%u: %s", KILN_SV(e.path), cap.code,
                   cap.errors, e.code, cap.msg);
}

} // namespace

KILN_TEST(Ktx2Corpus, Manifest) {
    char const* dir = kiln::test::corpus_dir();
    if (!dir) return; // no --corpus <dir>: nothing to check

    corpus::Manifest m;
    if (!corpus::load_manifest(dir, m)) return;

    u32 okCount = 0, unsupportedCount = 0;
    Vec<u8> bytes(default_allocator(), Tag::Test);
    for (corpus::Entry const& e : m.entries) {
        char path[1024];
        format(path, sizeof path, "%s/%.*s", dir, KILN_SV(e.path));
        bytes.clear();
        if (!KILN_CHECK_MSG(corpus::read_file(path, bytes), "manifest line %u: cannot read %s", e.line, path))
            continue;
        int const before = kiln::test::current_failures();
        if (e.expectOk) {
            check_ok_file(e, bytes.span());
            ++okCount;
        } else {
            check_unsupported_file(e, bytes.span());
            ++unsupportedCount;
        }
        if (kiln::test::current_failures() != before)
            std::fprintf(stderr, "  ^ corpus file %s (manifest line %u)\n", path, e.line);
    }
    KILN_CHECK(okCount > 0);
    KILN_CHECK(unsupportedCount > 0);
}
