// tests/test_ktx2_corpus_rt.cpp — KTX2 corpus read -> ktx2::write() -> re-open (cook side only).
// Description, level bytes and DFD must match; libktx's DFD (vk2dfd) and kiln's must be identical.
// Output goes to `<sample_dir()>/rt_<basename>` so CTest can run `ktx validate` on it.
#include "kiln_test.h"
#include "ktx2_corpus.h"

#include "kiln/containers.h"
#include "kiln/cook/ktx2_writer.h"
#include "kiln/ktx2.h"

using namespace kiln;
using namespace kiln::ktx2;
namespace corpus = kiln::test::corpus;

namespace {

bool writer_can_express(corpus::Entry const& e) {
    if (!e.expectOk || e.format == Format::Undefined) return false;
    FormatInfo const* info = format_info(e.format);
    bool const bc = u32(e.format) >= u32(Format::BC1_RGB_UNORM) && u32(e.format) <= u32(Format::BC7_SRGB);
    return info && (!info->compressed || bc) && e.depth == 0 && e.levels != 0 && e.supercompression == 0 &&
           (e.faces == 1 || (e.faces == 6 && e.layers == 0));
}

/// Report every differing DFD byte (up to a limit) as "offset: orig -> ours".
void report_dfd_diff(char const* name, Span<u8 const> orig, Span<u8 const> ours) {
    char msg[768];
    usize len     = format(msg, sizeof msg, "%s: DFD differs (original %zu bytes, kiln %zu bytes):", name,
                           orig.size, ours.size);
    usize const n = max(orig.size, ours.size);
    int shown     = 0;
    for (usize i = 0; i < n && len < sizeof msg - 32; ++i) {
        int const a = i < orig.size ? int(orig[i]) : -1;
        int const b = i < ours.size ? int(ours[i]) : -1;
        if (a == b) continue;
        if (++shown > 24) {
            len += format(msg + len, sizeof msg - len, " ...");
            break;
        }
        len += format(msg + len, sizeof msg - len, " [%zu] %02x->%02x", i, unsigned(a & 0xff),
                      unsigned(b & 0xff));
    }
    KILN_CHECK_MSG(false, "%s", msg);
}

void write_file(char const* dir, StrView path, Span<u8 const> bytes) {
    usize slash  = path.rfind('/');
    StrView base = slash == StrView::kNpos ? path : path.substr(slash + 1);
    char out[1024];
    format(out, sizeof out, "%s/rt_%.*s", dir, KILN_SV(base));
    std::FILE* f = std::fopen(out, "wb");
    if (!KILN_CHECK_MSG(f != nullptr, "fopen %s failed", out)) return;
    usize const written = bytes.size ? std::fwrite(bytes.data, 1, bytes.size, f) : 0;
    KILN_CHECK_MSG(written == bytes.size, "fwrite %s: wrote %zu of %zu bytes", out, written, bytes.size);
    std::fclose(f);
}

void round_trip(corpus::Entry const& e, Span<u8 const> bytes, char const* sampleDir) {
    char name[256];
    format(name, sizeof name, "%.*s", KILN_SV(e.path));

    Result<Ktx2View> r = Ktx2View::open(bytes, nullptr, e.path);
    if (!KILN_CHECK_MSG(r.ok(), "%s: open failed", name)) return;
    Ktx2View const& src = r.value();
    TextureDesc const d = src.desc();

    Span<u8 const> levels[kMaxLevels];
    for (u32 i = 0; i < d.levels; ++i) {
        levels[i] = src.level_data(i);
        if (!KILN_CHECK_MSG(!levels[i].empty(), "%s: level %u has no data", name, i)) return;
    }

    corpus::DiagCapture wcap;
    DiagSink wsink          = wcap.sink();
    WriteDesc const w       = {.format  = d.format,
                               .width   = d.width,
                               .height  = d.height,
                               .layers  = d.layers,
                               .faces   = d.faces,
                               .isArray = d.isArray,
                               .levels  = Span<Span<u8 const> const>(levels, d.levels)};
    Result<Vec<u8>> written = write(w, default_allocator(), &wsink);
    if (!KILN_CHECK_MSG(written.ok(), "%s: kiln write failed: %s", name, wcap.msg)) return;
    Vec<u8> const out = std::move(written).value();

    corpus::DiagCapture rcap;
    DiagSink rsink      = rcap.sink();
    Result<Ktx2View> rr = Ktx2View::open(out.span(), &rsink, "rt");
    if (!KILN_CHECK_MSG(rr.ok(), "%s: re-open of kiln output failed: %s", name, rcap.msg)) return;
    KILN_CHECK_MSG(rcap.count == 0, "%s: re-open diagnostics: %s", name, rcap.msg);
    Ktx2View const& rt  = rr.value();
    TextureDesc const o = rt.desc();

    KILN_CHECK_MSG(o.format == d.format && o.width == d.width && o.height == d.height && o.depth == d.depth &&
                       o.layers == d.layers && o.faces == d.faces && o.levels == d.levels &&
                       o.isArray == d.isArray && o.isCube == d.isCube,
                   "%s: round-tripped desc differs (%s %ux%u L%u -> %s %ux%u L%u)", name,
                   format_name(d.format), d.width, d.height, d.levels, format_name(o.format), o.width,
                   o.height, o.levels);
    KILN_CHECK_EQ(rt.header().typeSize, src.header().typeSize);
    KILN_CHECK_EQ(rt.header().levelCount, src.header().levelCount);

    // kiln-info --check equivalent on our output: every level present with the expected size.
    KILN_CHECK_MSG(rt.has_all_level_data(), "%s: kiln output lacks level data", name);
    for (u32 i = 0; i < o.levels; ++i) {
        u64 const want = rt.level_image_bytes(i) * o.faces * o.layers;
        KILN_CHECK_MSG(rt.levels()[i].byteLength == want && rt.level_data(i).size == want,
                       "%s: kiln output level %u is %llu bytes, expected %llu", name, i,
                       static_cast<unsigned long long>(rt.levels()[i].byteLength),
                       static_cast<unsigned long long>(want));
        KILN_CHECK_MSG(corpus::bytes_equal(rt.level_data(i), src.level_data(i)), "%s: level %u bytes differ",
                       name, i);
    }

    // The key real-world check: libktx (vk2dfd) and kiln must emit identical DFDs.
    if (!corpus::bytes_equal(src.dfd(), rt.dfd())) report_dfd_diff(name, src.dfd(), rt.dfd());

    if (sampleDir) write_file(sampleDir, e.path, out.span());
}

} // namespace

KILN_TEST(Ktx2Corpus, WriterRoundTrip) {
    char const* dir = kiln::test::corpus_dir();

    corpus::Manifest m;
    if (!corpus::load_manifest(dir, m)) return;

    u32 tripped = 0;
    Vec<u8> bytes(default_allocator(), Tag::Test);
    for (corpus::Entry const& e : m.entries) {
        if (!writer_can_express(e)) continue;
        char path[1024];
        format(path, sizeof path, "%s/%.*s", dir, KILN_SV(e.path));
        bytes.clear();
        if (!KILN_CHECK_MSG(corpus::read_file(path, bytes), "manifest line %u: cannot read %s", e.line, path))
            continue;
        int const before = kiln::test::current_failures();
        round_trip(e, bytes.span(), kiln::test::sample_dir());
        ++tripped;
        if (kiln::test::current_failures() != before)
            std::fprintf(stderr, "  ^ corpus file %s (manifest line %u)\n", path, e.line);
    }
    KILN_CHECK(tripped > 0);
}
