#include "kiln_test.h"

#include "kiln/cook/mesh_writer.h"
#include "kiln/mesh.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

using namespace kiln;
using namespace kiln::mesh;

namespace {

struct DiagCapture {
    u32 code          = 0;
    Severity severity = Severity::Info;
    Status status     = kOk;
    u32 count         = 0;
    char message[256]{};

    [[nodiscard]] DiagSink sink() noexcept { return DiagSink{&DiagCapture::on_diag, this}; }
    void reset() noexcept { *this = DiagCapture{}; }

    static void on_diag(void* user, Diagnostic const& d) {
        auto* self     = static_cast<DiagCapture*>(user);
        self->code     = d.code;
        self->severity = d.severity;
        self->status   = d.status;
        ++self->count;
        usize n = min(d.message.size, sizeof(self->message) - 1);
        if (n) std::memcpy(self->message, d.message.data, n);
        self->message[n] = '\0';
    }
};

VertexAttrib attr(Semantic sem, u8 semIndex, u8 stream, Format fmt, u16 offset) noexcept {
    VertexAttrib a{};
    a.semantic      = u8(sem);
    a.semanticIndex = semIndex;
    a.stream        = stream;
    a.format        = u32(fmt);
    a.offset        = offset;
    return a;
}

void fill_pattern(Vec<u8>& v, usize n, u32 seed) {
    v.resize(n);
    for (usize i = 0; i < n; ++i)
        v[i] = u8((u32(i) * 31u + seed * 17u) ^ (u32(i) >> 3));
}

void fill_u16(Vec<u8>& v, u32 count, u32 (*fn)(u32)) {
    v.resize(usize(count) * 2);
    for (u32 k = 0; k < count; ++k)
        write_unaligned(v.data() + usize(k) * 2, u16(fn(k)));
}
void fill_u32(Vec<u8>& v, u32 count, u32 (*fn)(u32)) {
    v.resize(usize(count) * 4);
    for (u32 k = 0; k < count; ++k)
        write_unaligned(v.data() + usize(k) * 4, fn(k));
}

Span<u8 const> cspan(Vec<u8> const& v) noexcept { return {v.data(), v.size()}; }

/// A small but complete mesh: 2 layouts, 3 parts (root, hull with 2 LODs, antenna),
/// 4 submeshes, 2 materials, 3 textures, 3 mounts (given unsorted).
/// LOD records: 0 = hull LOD0, 1 = hull LOD1, 2 = antenna LOD0.
struct TestMesh {
    VertexLayout layouts[2]{};
    PartDesc parts[3]{};
    LodDesc lods[3]{};
    Submesh submeshes[4]{};
    MaterialDesc materials[2]{};
    TextureBindingDesc textures[3]{};
    MountDesc mounts[3]{};
    Vec<u8> streams[3][2];
    Vec<u8> indices[3];

    TestMesh() {
        // Layout 0 "default": 8 B position + 16 B normal/tangent/uv0.
        VertexLayout& l0 = layouts[0];
        l0.streamCount   = 2;
        l0.attribCount   = 4;
        l0.strides[0]    = 8;
        l0.strides[1]    = 16;
        l0.attribs[0]    = attr(Semantic::Position, 0, 0, Format::R16G16B16A16_UNORM, 0);
        l0.attribs[1]    = attr(Semantic::Normal, 0, 1, Format::R16G16_SNORM, 0);
        l0.attribs[2]    = attr(Semantic::Tangent, 0, 1, Format::R16G16B16A16_SNORM, 4);
        l0.attribs[3]    = attr(Semantic::TexCoord, 0, 1, Format::R16G16_SFLOAT, 12);
        // Layout 1 "precise": float position + float uv0.
        VertexLayout& l1 = layouts[1];
        l1.streamCount   = 2;
        l1.attribCount   = 2;
        l1.strides[0]    = 12;
        l1.strides[1]    = 8;
        l1.attribs[0]    = attr(Semantic::Position, 0, 0, Format::R32G32B32_SFLOAT, 0);
        l1.attribs[1]    = attr(Semantic::TexCoord, 0, 1, Format::R32G32_SFLOAT, 0);

        parts[0].name           = "root";
        parts[1].name           = "hull";
        parts[1].parent         = 0;
        parts[1].translation[1] = 1.5f;
        parts[1].lodFirst       = 0;
        parts[1].lodCount       = 2;
        parts[1].posScale[0]    = 4.0f;
        parts[2].name           = "antenna";
        parts[2].parent         = 1;
        parts[2].lodFirst       = 2;
        parts[2].lodCount       = 1;

        // hull LOD0: 40 vertices, 60 U16 indices, 2 submeshes (the second with vertexBase 4).
        fill_pattern(streams[0][0], 40 * 8, 1);
        fill_pattern(streams[0][1], 40 * 16, 2);
        fill_u16(indices[0], 60, [](u32 k) { return k < 30 ? k % 40u : (k * 7u) % 36u; });
        // hull LOD1: 12 vertices, 18 U16 indices.
        fill_pattern(streams[1][0], 12 * 8, 3);
        fill_pattern(streams[1][1], 12 * 16, 4);
        fill_u16(indices[1], 18, [](u32 k) { return k % 12u; });
        // antenna LOD0: 9 vertices, 12 U32 indices.
        fill_pattern(streams[2][0], 9 * 12, 5);
        fill_pattern(streams[2][1], 9 * 8, 6);
        fill_u32(indices[2], 12, [](u32 k) { return (k * 5u) % 9u; });

        u32 const vcount[3]      = {40, 12, 9};
        u32 const icount[3]      = {60, 18, 12};
        IndexType const itype[3] = {IndexType::U16, IndexType::U16, IndexType::U32};
        u32 const layout[3]      = {0, 0, 1};
        u32 const smFirst[3]     = {0, 2, 3};
        u32 const smCount[3]     = {2, 1, 1};
        for (u32 i = 0; i < 3; ++i) {
            LodDesc& l     = lods[i];
            l.layout       = layout[i];
            l.vertexCount  = vcount[i];
            l.streams[0]   = cspan(streams[i][0]);
            l.streams[1]   = cspan(streams[i][1]);
            l.indices      = cspan(indices[i]);
            l.indexCount   = icount[i];
            l.indexType    = itype[i];
            l.submeshFirst = smFirst[i];
            l.submeshCount = smCount[i];
        }
        lods[1].geometricError = 0.25f;

        submeshes[0]               = Submesh{0, 0, 30, 0, {}};
        submeshes[1]               = Submesh{1, 30, 30, 4, {}};
        submeshes[2]               = Submesh{0, 0, 18, 0, {}};
        submeshes[3]               = Submesh{1, 0, 12, 0, {}};
        submeshes[1].bounds.radius = 2.0f;

        materials[0].name         = "hull_paint";
        materials[0].textureFirst = 0;
        materials[0].textureCount = 3;
        materials[1].name         = "glass";
        materials[1].alphaMode    = AlphaMode::Blend;
        materials[1].alphaCutoff  = 0.0f;

        textures[0] =
            TextureBindingDesc{"textures/hull_albedo", 0, TextureSlot::BaseColor, 0, u16(kTextureSrgb)};
        textures[1] = TextureBindingDesc{"textures/hull_normal", 0, TextureSlot::Normal, 0, 0};
        textures[2] = TextureBindingDesc{"textures/hull_mr", 0, TextureSlot::MetalRough, 0, 0};

        mounts[0].name           = "mount_engine_01";
        mounts[0].parentPart     = 1;
        mounts[0].translation[2] = -3.0f;
        mounts[1].name           = "mount_engine_00";
        mounts[1].parentPart     = 1;
        mounts[2].name           = "mount_gun";
        mounts[2].parentPart     = 1;
        mounts[2].extras         = "slot=hardpoint;size=2";
    }
    TestMesh(TestMesh const&)            = delete;
    TestMesh& operator=(TestMesh const&) = delete;

    [[nodiscard]] WriteDesc desc() const noexcept {
        WriteDesc d;
        d.name       = "ships/test_hauler";
        d.sourceHash = 0x1122334455667788ull;
        d.cookHash   = 0x99aabbccddeeff00ull;
        d.layouts    = layouts;
        d.parts      = parts;
        d.lods       = lods;
        d.submeshes  = submeshes;
        d.materials  = materials;
        d.textures   = textures;
        d.mounts     = mounts;
        return d;
    }
};

Result<MeshView> open_bytes(Vec<u8> const& b, OpenOptions const& opt = {}, DiagSink const* diag = nullptr) {
    return MeshView::open(cspan(b), opt, diag);
}

/// Decode the payload and compare every LOD range with the writer input.
void check_payload_matches(MeshView const& v, TestMesh const& m, DecodeOptions const& dopt = {true}) {
    Vec<u8> dst(nullptr, Tag::Test);
    dst.resize(usize(v.decoded_size()), u8(0xCD));
    DiagCapture cap;
    DiagSink sink = cap.sink();
    Status st     = decode_payload(v, v.encoded(), dst.span(), dopt, &sink);
    KILN_CHECK_MSG(st.ok(), "decode_payload: %s", cap.message);
    if (st.failed()) return;
    KILN_REQUIRE_EQ(v.lods().size(), 3u);
    for (u32 li = 0; li < 3; ++li) {
        MeshLod const& l = v.lods()[li];
        for (u32 s = 0; s < 2; ++s) {
            Vec<u8> const& want = m.streams[li][s];
            KILN_CHECK_EQ(v.stream_bytes(l, s), u64(want.size()));
            KILN_CHECK_MSG(std::memcmp(dst.data() + l.streamOffset[s], want.data(), want.size()) == 0,
                           "lod %u stream %u bytes differ", li, s);
        }
        Vec<u8> const& wantIdx = m.indices[li];
        KILN_CHECK_EQ(MeshView::index_bytes(l), u64(wantIdx.size()));
        KILN_CHECK_MSG(std::memcmp(dst.data() + l.indexOffset, wantIdx.data(), wantIdx.size()) == 0,
                       "lod %u index bytes differ", li);
    }
    // Gaps of the decoded payload (bytes covered by no blob) are zero.
    Vec<u8> covered(nullptr, Tag::Test);
    covered.resize(dst.size(), u8(0));
    for (PayloadBlob const& b : v.blobs())
        for (u32 k = 0; k < b.decodedSize; ++k)
            covered[b.decodedOffset + k] = 1;
    u32 gapNonZero = 0;
    for (usize i = 0; i < dst.size(); ++i)
        if (!covered[i] && dst[i] != 0) ++gapNonZero;
    KILN_CHECK_EQ(gapNonZero, 0u);
    KILN_CHECK_MSG(check_indices(v, dst.span(), &sink).ok(), "check_indices: %s", cap.message);
}

/// Bytes of GPUD not covered by any encoded blob range must be zero in the file.
u32 nonzero_gpud_gaps(MeshView const& v) {
    Span<u8 const> g = v.encoded();
    Vec<u8> covered(nullptr, Tag::Test);
    covered.resize(g.size, u8(0));
    for (PayloadBlob const& b : v.blobs())
        for (u32 k = 0; k < b.encodedSize; ++k)
            covered[b.encodedOffset + k] = 1;
    u32 n = 0;
    for (usize i = 0; i < g.size; ++i)
        if (!covered[i] && g[i] != 0) ++n;
    return n;
}

/// Offset of a section's data, found by scanning the raw section table.
u64 section_offset(Vec<u8> const& b, u32 id) {
    FileHeader h = read_unaligned<FileHeader>(b.data());
    for (u32 i = 0; i < h.sectionCount; ++i) {
        SectionEntry e =
            read_unaligned<SectionEntry>(b.data() + h.sectionTableOffset + usize(i) * sizeof(SectionEntry));
        if (e.id == id) return e.offset;
    }
    return ~u64(0);
}

/// Read record `index` of type T in section `id`, let `fn` modify it, write it back.
template <class T, class F> void patch_record(Vec<u8>& b, u32 id, u32 index, F&& fn) {
    u64 off = section_offset(b, id);
    KILN_VERIFY(off != ~u64(0));
    u8* p = b.data() + off + usize(index) * sizeof(T);
    T r   = read_unaligned<T>(p);
    fn(r);
    write_unaligned(p, r);
}
template <class F> void patch_header(Vec<u8>& b, F&& fn) {
    FileHeader h = read_unaligned<FileHeader>(b.data());
    fn(h);
    write_unaligned(b.data(), h);
}

void expect_open_fails(Vec<u8> const& b, Code code, u32 diagCode, char const* what,
                       OpenOptions const& opt = {}, usize size = ~usize(0)) {
    DiagCapture cap;
    DiagSink sink = cap.sink();
    auto r        = MeshView::open(Span<u8 const>(b.data(), size == ~usize(0) ? b.size() : size), opt, &sink);
    KILN_CHECK_MSG(r.failed(), "%s: open unexpectedly succeeded", what);
    KILN_CHECK_MSG(r.code() == code, "%s: status %s, expected %s (%s)", what, code_name(r.code()),
                   code_name(code), cap.message);
    KILN_CHECK_MSG(cap.code == diagCode, "%s: diag K%u, expected K%u (%s)", what, cap.code, diagCode,
                   cap.message);
    KILN_CHECK_MSG(cap.severity == Severity::Error, "%s: severity", what);
}

Vec<u8> write_ok(WriteDesc const& d, WriteOptions const& opt = {}, WriteStats* stats = nullptr) {
    DiagCapture cap;
    DiagSink sink = cap.sink();
    auto r        = write(d, opt, nullptr, &sink, stats);
    KILN_CHECK_MSG(r.ok(), "write failed: %s", cap.message);
    if (r.failed()) return Vec<u8>();
    return std::move(r).value();
}

/// Write `bytes` to `<dir>/<name>` with plain C stdio, checking that fopen/fwrite succeed.
void write_sample_file(char const* dir, char const* name, Span<u8 const> bytes) {
    char path[1024];
    format(path, sizeof path, "%s/%s", dir, name);
    std::FILE* f = std::fopen(path, "wb");
    if (!KILN_CHECK_MSG(f != nullptr, "fopen %s failed", path)) return;
    usize written = bytes.size ? std::fwrite(bytes.data, 1, bytes.size, f) : 0;
    KILN_CHECK_MSG(written == bytes.size, "fwrite %s: wrote %zu of %zu bytes", path, written, bytes.size);
    std::fclose(f);
}

} // namespace

KILN_TEST(Mesh, RoundTripRaw) {
    TestMesh m;
    WriteStats stats;
    DiagCapture wcap;
    DiagSink wsink = wcap.sink();
    auto wr        = write(m.desc(), {}, nullptr, &wsink, &stats);
    KILN_REQUIRE(wr.ok());
    KILN_CHECK_EQ(wcap.count, 0u); // no warnings for a fully referenced mesh
    Vec<u8> const& bytes = *wr;
    KILN_CHECK(stats.raw);
    KILN_CHECK_EQ(stats.blobCount, 9u);
    KILN_CHECK_EQ(stats.fileSize, u64(bytes.size()));

    DiagCapture cap;
    DiagSink sink = cap.sink();
    auto r        = open_bytes(bytes, {}, &sink);
    KILN_REQUIRE(r.ok());
    MeshView const& v = *r;

    // Header
    FileHeader const& h = v.header();
    KILN_CHECK_EQ(h.magic, kMagic);
    KILN_CHECK_EQ(h.versionMajor, kVersionMajor);
    KILN_CHECK_EQ(h.versionMinor, kVersionMinor);
    KILN_CHECK_EQ(h.fileSize, u64(bytes.size()));
    KILN_CHECK_EQ(h.sectionTableOffset, u64(80));
    KILN_CHECK_EQ(h.gpuDataOffset % 256, u64(0));
    KILN_CHECK_EQ(h.gpuDataSize, h.fileSize - h.gpuDataOffset);
    KILN_CHECK_EQ(h.gpuDataSize, h.payloadDecodedSize);
    KILN_CHECK_EQ(h.payloadDecodedSize, stats.decodedSize);
    KILN_CHECK_EQ(h.payloadAlignment, 256u);
    KILN_CHECK_EQ(h.flags, u32(kPayloadRaw));
    KILN_CHECK_EQ(h.sourceHash, u64(0x1122334455667788ull));
    KILN_CHECK_EQ(h.cookHash, u64(0x99aabbccddeeff00ull));
    KILN_CHECK_EQ(h._reserved, 0u);
    KILN_CHECK(v.payload_raw());
    KILN_CHECK_EQ(v.encoded().size, usize(h.gpuDataSize));

    // Sections: order, count, alignment, strides
    u32 const expected[] = {kSecModel,     kSecStrings,  kSecLayouts, kSecParts, kSecLods,   kSecSubmeshes,
                            kSecMaterials, kSecTextures, kSecMounts,  kSecBlobs, kSecGpuData};
    KILN_REQUIRE_EQ(v.sections().size, countof(expected));
    KILN_CHECK_EQ(h.sectionCount, u32(countof(expected)));
    for (usize i = 0; i < countof(expected); ++i) {
        SectionEntry const e = v.sections()[i];
        KILN_CHECK_EQ(e.id, expected[i]);
        KILN_CHECK_EQ(e.flags, 0u);
        KILN_CHECK_EQ(e.offset % 16, u64(0));
        if (e.id == kSecStrings || e.id == kSecGpuData) {
            KILN_CHECK_EQ(e.count, 0u);
            KILN_CHECK_EQ(e.stride, 0u);
        } else {
            KILN_CHECK_EQ(e.size, u64(e.count) * e.stride);
        }
        if (i > 0) KILN_CHECK(e.offset >= v.sections()[i - 1].offset + v.sections()[i - 1].size);
    }
    KILN_CHECK_EQ(v.find_section(kSecGpuData)->offset, h.gpuDataOffset);
    KILN_CHECK_EQ(v.find_section(kSecParts)->stride, u32(sizeof(MeshPart)));

    // Model
    KILN_CHECK_EQ(v.name(), StrView("ships/test_hauler"));
    KILN_CHECK_EQ(v.asset_id(), hash_name("ships/test_hauler"));
    KILN_CHECK_EQ(v.str(0), StrView(""));
    KILN_CHECK_EQ(v.strings()[0], u8(0));
    KILN_CHECK_EQ(v.strings().size % 16, usize(0));

    // Records
    KILN_CHECK_EQ(v.layouts().size(), 2u);
    KILN_CHECK_EQ(v.parts().size(), 3u);
    KILN_CHECK_EQ(v.lods().size(), 3u);
    KILN_CHECK_EQ(v.submeshes().size(), 4u);
    KILN_CHECK_EQ(v.materials().size(), 2u);
    KILN_CHECK_EQ(v.textures().size(), 3u);
    KILN_CHECK_EQ(v.mounts().size(), 3u);
    KILN_CHECK_EQ(v.blobs().size(), 9u);
    KILN_CHECK(v.parts().contiguous());

    KILN_CHECK_EQ(v.layouts()[0].strides[1], u16(16));
    KILN_CHECK_EQ(v.layouts()[1].attribs[0].format, u32(Format::R32G32B32_SFLOAT));
    KILN_CHECK_EQ(v.str(v.parts()[0].nameStr), StrView("root"));
    KILN_CHECK_EQ(v.str(v.parts()[1].nameStr), StrView("hull"));
    KILN_CHECK_EQ(v.parts()[0].parent, kInvalid);
    KILN_CHECK_EQ(v.parts()[0].lodCount, 0u);
    KILN_CHECK_EQ(v.parts()[1].parent, 0u);
    KILN_CHECK_EQ(v.parts()[1].nameHash, hash_name("hull"));
    KILN_CHECK_EQ(v.parts()[1].translation[1], 1.5f);
    KILN_CHECK_EQ(v.parts()[1].posScale[0], 4.0f);
    KILN_CHECK_EQ(v.parts()[1].rotation[3], 1.0f);
    KILN_CHECK_EQ(v.parts()[2].parent, 1u);
    KILN_CHECK_EQ(v.parts()[2].lodFirst, 2u);
    KILN_CHECK(v.find_part(hash_name("antenna")) == &v.parts()[2]);
    KILN_CHECK(v.find_part(hash_name("nope")) == nullptr);

    KILN_CHECK_EQ(v.lods()[0].vertexCount, 40u);
    KILN_CHECK_EQ(v.lods()[1].geometricError, 0.25f);
    KILN_CHECK_EQ(v.lods()[2].indexType, u8(IndexType::U32));
    KILN_CHECK_EQ(v.lods()[2].streamOffset[2], kInvalid);
    KILN_CHECK_EQ(v.lods()[2].streamOffset[3], kInvalid);
    KILN_CHECK_EQ(v.submeshes()[1].vertexBase, 4);
    KILN_CHECK_EQ(v.submeshes()[1].indexFirst, 30u);
    KILN_CHECK_EQ(v.submeshes()[1].bounds.radius, 2.0f);

    KILN_CHECK_EQ(v.str(v.materials()[0].nameStr), StrView("hull_paint"));
    KILN_CHECK_EQ(v.materials()[0].nameHash, hash_name("hull_paint"));
    KILN_CHECK_EQ(v.materials()[0].textureCount, 3u);
    KILN_CHECK_EQ(v.materials()[1].alphaMode, u8(AlphaMode::Blend));
    KILN_CHECK_EQ(v.materials()[1].alphaCutoff, 0.0f);
    KILN_CHECK_EQ(v.str(v.textures()[1].pathStr), StrView("textures/hull_normal"));
    KILN_CHECK_EQ(v.textures()[0].textureId, hash_name("textures/hull_albedo"));
    KILN_CHECK_EQ(v.textures()[0].flags, u16(kTextureSrgb));
    KILN_CHECK_EQ(v.textures()[2].slot, u8(TextureSlot::MetalRough));

    // Mounts: sorted, searchable, extras resolve
    for (u32 i = 1; i < v.mounts().size(); ++i)
        KILN_CHECK(v.mounts()[i - 1].nameHash <= v.mounts()[i].nameHash);
    Mount const* gun = v.find_mount(hash_name("mount_gun"));
    KILN_REQUIRE(gun != nullptr);
    KILN_CHECK_EQ(v.str(gun->nameStr), StrView("mount_gun"));
    KILN_CHECK_EQ(v.str(gun->extrasStr), StrView("slot=hardpoint;size=2"));
    KILN_CHECK_EQ(gun->parentPart, 1u);
    Mount const* e1 = v.find_mount(hash_name("mount_engine_01"));
    KILN_REQUIRE(e1 != nullptr);
    KILN_CHECK_EQ(e1->extrasStr, kInvalid);
    KILN_CHECK_EQ(e1->translation[2], -3.0f);
    KILN_CHECK(v.find_mount(123) == nullptr);

    // Blob table: sorted by encodedOffset, non-decreasing rank, coarsest first
    auto const& blobs = v.blobs();
    KILN_CHECK_EQ(blobs[0].lodRank, u8(0));
    KILN_CHECK_EQ(blobs[0].decodedOffset, v.lods()[1].streamOffset[0]); // hull LOD1 (rank 0, lowest index)
    KILN_CHECK_EQ(blobs[0].decodedOffset, 0u);
    KILN_CHECK_EQ(blobs[8].lodRank, u8(1)); // hull LOD0 indices last
    for (u32 i = 0; i < blobs.size(); ++i) {
        PayloadBlob const& b = blobs[i];
        if (i > 0) {
            KILN_CHECK(b.encodedOffset >= blobs[i - 1].encodedOffset + blobs[i - 1].encodedSize);
            KILN_CHECK(b.lodRank >= blobs[i - 1].lodRank);
        }
        KILN_CHECK_EQ(b.encodedOffset % 16, 0u);
        KILN_CHECK_EQ(b.decodedOffset % 16, 0u);
        KILN_CHECK_EQ(b.encodedOffset, b.decodedOffset);
        KILN_CHECK_EQ(b.encodedSize, b.decodedSize);
        KILN_CHECK_EQ(b.codec, u8(Codec::None));
        KILN_CHECK_EQ(b.filter, u8(Filter::None));
        KILN_CHECK_EQ(b.flags, u16(0));
        KILN_CHECK_EQ(b._pad, u8(0));
        KILN_CHECK_EQ(b._reserved, 0u);
        KILN_CHECK_NE(b.checksum, 0u);
        KILN_CHECK(b.elementSize != 0 && b.decodedSize % b.elementSize == 0);
    }
    KILN_CHECK_EQ(nonzero_gpud_gaps(v), 0u);

    check_payload_matches(v, m);
}

KILN_TEST(Mesh, RoundTripPadded) {
    TestMesh m;
    WriteOptions opt;
    opt.encodedPadding = 32;
    WriteStats stats;
    Vec<u8> const bytes = write_ok(m.desc(), opt, &stats);
    KILN_REQUIRE(!bytes.empty());
    KILN_CHECK(!stats.raw);
    KILN_CHECK_EQ(stats.blobCount, 9u);

    auto r = open_bytes(bytes);
    KILN_REQUIRE(r.ok());
    MeshView const& v = *r;
    KILN_CHECK(!v.payload_raw());
    KILN_CHECK_EQ(v.header().flags, 0u);
    KILN_CHECK(v.header().gpuDataSize > v.header().payloadDecodedSize);
    u32 differing = 0;
    for (PayloadBlob const& b : v.blobs()) {
        if (b.encodedOffset != b.decodedOffset) ++differing;
        KILN_CHECK_EQ(b.encodedSize, b.decodedSize);
        KILN_CHECK_EQ(b.encodedOffset % 16, 0u);
    }
    KILN_CHECK_EQ(differing, v.blobs().size());
    KILN_CHECK_EQ(nonzero_gpud_gaps(v), 0u);
    check_payload_matches(v, m);

    // Same decoded layout as the raw file.
    Vec<u8> const rawBytes = write_ok(m.desc());
    auto rr                = open_bytes(rawBytes);
    KILN_REQUIRE(rr.ok());
    KILN_CHECK_EQ(rr->decoded_size(), v.decoded_size());
    for (u32 i = 0; i < v.blobs().size(); ++i)
        KILN_CHECK_EQ(rr->blobs()[i].decodedOffset, v.blobs()[i].decodedOffset);
}

KILN_TEST(Mesh, Deterministic) {
    TestMesh m;
    Vec<u8> const a = write_ok(m.desc());
    Vec<u8> const b = write_ok(m.desc());
    KILN_REQUIRE(!a.empty());
    KILN_REQUIRE_EQ(a.size(), b.size());
    KILN_CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);

    WriteOptions opt;
    opt.checksums   = false;
    Vec<u8> const c = write_ok(m.desc(), opt);
    KILN_REQUIRE_EQ(c.size(), a.size());
    auto r = open_bytes(c);
    KILN_REQUIRE(r.ok());
    for (PayloadBlob const& blob : r->blobs())
        KILN_CHECK_EQ(blob.checksum, 0u);
    check_payload_matches(*r, m);
}

KILN_TEST(Mesh, ChecksumMismatch) {
    TestMesh m;
    Vec<u8> bytes = write_ok(m.desc());
    KILN_REQUIRE(!bytes.empty());
    u64 gpuOff               = read_unaligned<FileHeader>(bytes.data()).gpuDataOffset;
    bytes[usize(gpuOff) + 5] = u8(bytes[usize(gpuOff) + 5] ^ 0xFFu);

    auto r = open_bytes(bytes);
    KILN_REQUIRE(r.ok()); // open does not look at payload bytes
    Vec<u8> dst(nullptr, Tag::Test);
    dst.resize(usize(r->decoded_size()));
    DiagCapture cap;
    DiagSink sink = cap.sink();
    Status st     = decode_payload(*r, r->encoded(), dst.span(), DecodeOptions{true}, &sink);
    KILN_CHECK_EQ(st.code, Code::Corrupt);
    KILN_CHECK_EQ(cap.code, u32(kDiagChecksum));
    KILN_CHECK(decode_payload(*r, r->encoded(), dst.span(), DecodeOptions{false}).ok());

    // Same on the non-raw decode loop.
    WriteOptions opt;
    opt.encodedPadding = 16;
    Vec<u8> padded     = write_ok(m.desc(), opt);
    KILN_REQUIRE(!padded.empty());
    auto rp = open_bytes(padded);
    KILN_REQUIRE(rp.ok());
    {
        usize at   = usize(rp->header().gpuDataOffset + rp->blobs()[3].encodedOffset);
        padded[at] = u8(padded[at] ^ 0x01u);
    }
    cap.reset();
    dst.resize(usize(rp->decoded_size()));
    st = decode_payload(*rp, rp->encoded(), dst.span(), DecodeOptions{true}, &sink);
    KILN_CHECK_EQ(st.code, Code::Corrupt);
    KILN_CHECK_EQ(cap.code, u32(kDiagChecksum));
    KILN_CHECK(decode_payload(*rp, rp->encoded(), dst.span(), DecodeOptions{false}).ok());
}

KILN_TEST(Mesh, CorruptionsDetected) {
    TestMesh m;
    Vec<u8> const good = write_ok(m.desc());
    KILN_REQUIRE(!good.empty());
    KILN_REQUIRE(open_bytes(good).ok());
    WriteOptions padOpt;
    padOpt.encodedPadding = 32;
    Vec<u8> const padded  = write_ok(m.desc(), padOpt);
    KILN_REQUIRE(open_bytes(padded).ok());

    {
        Vec<u8> b = good.clone();
        patch_header(b, [](FileHeader& h) { h.magic = fourcc("OMSH"); });
        expect_open_fails(b, Code::Corrupt, kDiagBadMagic, "bad magic");
    }
    {
        Vec<u8> b = good.clone();
        patch_header(b, [](FileHeader& h) { h.versionMinor = u16(h.versionMinor + 1); });
        expect_open_fails(b, Code::VersionMismatch, kDiagVersion, "minor version");
    }
    {
        u64 cpu = read_unaligned<FileHeader>(good.data()).gpuDataOffset;
        expect_open_fails(good, Code::Corrupt, kDiagTruncated, "truncated", {}, usize(cpu / 2));
    }
    {
        Vec<u8> b = good.clone();
        patch_header(b, [](FileHeader& h) { h.gpuDataSize += 16; });
        expect_open_fails(b, Code::Corrupt, kDiagHeaderSizes, "gpuDataSize");
    }
    { // found by fuzz_mesh_read: the table end wrapped past 2^64 and passed the bounds check
        Vec<u8> b = good.clone();
        patch_header(b, [](FileHeader& h) { h.sectionTableOffset = ~u64(0) - 79; });
        expect_open_fails(b, Code::Corrupt, kDiagSectionTable, "section table offset wraps");
    }
    {
        Vec<u8> b = good.clone();
        patch_record<MeshPart>(b, kSecParts, 2, [](MeshPart& p) { p.parent = 2; });
        expect_open_fails(b, Code::ValidationFailed, kDiagPartOrder, "part parent = self");
        // With validation off only the structural checks run.
        OpenOptions noValidate;
        noValidate.validate = false;
        KILN_CHECK(MeshView::open(cspan(b), noValidate).ok());
    }
    {
        Vec<u8> b = good.clone();
        patch_record<MeshLod>(b, kSecLods, 1, [](MeshLod& l) { l.layout = 99; });
        expect_open_fails(b, Code::Corrupt, kDiagIndexRange, "lod layout");
    }
    {
        // Pull the second blob back into the first: the table is no longer sorted /
        // the ranges overlap. (The raw identity check runs after the ordering check.)
        Vec<u8> b = good.clone();
        patch_record<PayloadBlob>(b, kSecBlobs, 1, [](PayloadBlob& p) { p.encodedOffset -= 16; });
        expect_open_fails(b, Code::Corrupt, kDiagBlobTable, "blob overlap");
    }
    {
        Vec<u8> b = good.clone();
        patch_record<PayloadBlob>(b, kSecBlobs, 0, [](PayloadBlob& p) { p.codec = 7; });
        expect_open_fails(b, Code::Unsupported, kDiagBlobUnsupported, "unknown codec");
    }
    {
        Vec<u8> b = good.clone();
        patch_record<PayloadBlob>(b, kSecBlobs, 0, [](PayloadBlob& p) { p.filter = u8(Filter::MeshoptOct); });
        expect_open_fails(b, Code::ValidationFailed, kDiagBlobEncoding, "codec None + MeshoptOct");
    }
    {
        Vec<u8> b = padded.clone();
        patch_header(b, [](FileHeader& h) { h.flags |= kPayloadRaw; });
        expect_open_fails(b, Code::Corrupt, kDiagPayloadRaw, "kPayloadRaw on padded file");
    }
    {
        Vec<u8> b = good.clone();
        Mount m0{}, m1{};
        patch_record<Mount>(b, kSecMounts, 0, [&](Mount& x) { m0 = x; });
        patch_record<Mount>(b, kSecMounts, 1, [&](Mount& x) {
            m1 = x;
            x  = m0;
        });
        patch_record<Mount>(b, kSecMounts, 0, [&](Mount& x) { x = m1; });
        expect_open_fails(b, Code::ValidationFailed, kDiagMountOrder, "mounts swapped");
    }
    {
        Vec<u8> b = good.clone();
        auto r    = open_bytes(good);
        KILN_REQUIRE(r.ok());
        SectionEntry const* s = r->find_section(kSecStrings);
        KILN_REQUIRE(s != nullptr);
        b[usize(s->offset + s->size - 1)] = u8('x');
        expect_open_fails(b, Code::Corrupt, kDiagStringOffset, "STRS without final NUL");
    }
}

namespace {
void expect_write_fails(WriteDesc const& d, u32 diagCode, char const* what, WriteOptions const& opt = {}) {
    DiagCapture cap;
    DiagSink sink = cap.sink();
    auto r        = write(d, opt, nullptr, &sink);
    KILN_CHECK_MSG(r.failed(), "%s: write unexpectedly succeeded", what);
    KILN_CHECK_MSG(r.code() == Code::InvalidArgument, "%s: status %s", what, code_name(r.code()));
    KILN_CHECK_MSG(cap.code == diagCode, "%s: diag K%u, expected K%u (%s)", what, cap.code, diagCode,
                   cap.message);
}
} // namespace

KILN_TEST(Mesh, WriterRejectsBadInput) {
    TestMesh m;
    {
        LodDesc lods[3]    = {m.lods[0], m.lods[1], m.lods[2]};
        lods[0].streams[1] = lods[0].streams[1].first(lods[0].streams[1].size - 16);
        WriteDesc d        = m.desc();
        d.lods             = lods;
        expect_write_fails(d, kDiagLodRange, "stream size mismatch");
    }
    {
        LodDesc lods[3] = {m.lods[0], m.lods[1], m.lods[2]};
        lods[2].layout  = 2;
        WriteDesc d     = m.desc();
        d.lods          = lods;
        expect_write_fails(d, kDiagIndexRange, "missing layout");
    }
    {
        Submesh subs[4]    = {m.submeshes[0], m.submeshes[1], m.submeshes[2], m.submeshes[3]};
        subs[2].indexFirst = 3; // 3 + 18 > 18
        WriteDesc d        = m.desc();
        d.submeshes        = subs;
        expect_write_fails(d, kDiagIndexRange, "submesh index range");
    }
    {
        VertexLayout lays[2]      = {m.layouts[0], m.layouts[1]};
        lays[0].attribs[1].stream = 0; // Normal in stream 0
        WriteDesc d               = m.desc();
        d.layouts                 = lays;
        expect_write_fails(d, kDiagLayoutRule, "two attributes in stream 0");
    }
    {
        PartDesc parts[3] = {m.parts[0], m.parts[1], m.parts[2]};
        parts[1].parent   = 1;
        WriteDesc d       = m.desc();
        d.parts           = parts;
        expect_write_fails(d, kDiagPartOrder, "part parent = self");
    }
    {
        MountDesc mounts[3]  = {m.mounts[0], m.mounts[1], m.mounts[2]};
        mounts[1].parentPart = 3;
        WriteDesc d          = m.desc();
        d.mounts             = mounts;
        expect_write_fails(d, kDiagIndexRange, "mount parent");
    }
    {
        WriteOptions opt;
        opt.payloadAlignment = 384;
        expect_write_fails(m.desc(), kDiagHeaderSizes, "payloadAlignment", opt);
    }
    {
        WriteDesc d = m.desc();
        d.parts     = {};
        expect_write_fails(d, kDiagSectionMissing, "no parts");
    }
    // Write through a null diag sink works too.
    WriteDesc d = m.desc();
    d.layouts   = {};
    auto r      = write(d);
    KILN_CHECK_EQ(r.code(), Code::InvalidArgument);
}

KILN_TEST(Mesh, UnreferencedLodWarns) {
    TestMesh m;
    PartDesc parts[3] = {m.parts[0], m.parts[1], m.parts[2]};
    parts[2].lodCount = 0; // antenna LOD becomes orphaned
    WriteDesc d       = m.desc();
    d.parts           = parts;
    DiagCapture cap;
    DiagSink sink = cap.sink();
    auto w        = write(d, {}, nullptr, &sink);
    KILN_REQUIRE(w.ok());
    KILN_CHECK_EQ(cap.count, 1u);
    KILN_CHECK_EQ(cap.code, u32(kDiagIndexRange));
    KILN_CHECK(cap.severity == Severity::Warning);
    auto r = open_bytes(*w);
    KILN_REQUIRE(r.ok());
    KILN_CHECK_EQ(r->blobs()[r->blobs().size() - 1].lodRank, u8(255));
    check_payload_matches(*r, m);
}

KILN_TEST(Mesh, NoIndicesNoTextures) {
    Vec<u8> pos(nullptr, Tag::Test);
    fill_pattern(pos, 5 * 12, 9);

    VertexLayout lay{};
    lay.streamCount = 1;
    lay.attribCount = 1;
    lay.strides[0]  = 12;
    lay.attribs[0]  = attr(Semantic::Position, 0, 0, Format::R32G32B32_SFLOAT, 0);
    PartDesc part;
    part.name     = "points";
    part.lodCount = 1;
    LodDesc lod;
    lod.layout      = 0;
    lod.vertexCount = 5;
    lod.streams[0]  = cspan(pos);

    WriteDesc d;
    d.name    = "points";
    d.assetId = 42;
    d.layouts = Span<VertexLayout const>(&lay, 1);
    d.parts   = Span<PartDesc const>(&part, 1);
    d.lods    = Span<LodDesc const>(&lod, 1);

    WriteStats stats;
    Vec<u8> const bytes = write_ok(d, {}, &stats);
    KILN_REQUIRE(!bytes.empty());
    KILN_CHECK_EQ(stats.blobCount, 1u);
    KILN_CHECK(stats.raw);

    auto r = open_bytes(bytes);
    KILN_REQUIRE(r.ok());
    MeshView const& v = *r;
    KILN_CHECK(v.find_section(kSecTextures) == nullptr);
    KILN_CHECK(v.find_section(kSecMounts) == nullptr);
    KILN_CHECK(v.find_section(kSecSubmeshes) != nullptr);
    KILN_CHECK_EQ(v.header().sectionCount, 9u);
    KILN_CHECK_EQ(v.asset_id(), u64(42));
    KILN_CHECK(v.textures().empty());
    KILN_CHECK(v.mounts().empty());
    KILN_CHECK(v.submeshes().empty());
    KILN_CHECK_EQ(v.lods()[0].indexCount, 0u);
    KILN_CHECK_EQ(v.lods()[0].streamOffset[1], kInvalid);
    KILN_CHECK(v.find_mount(hash_name("x")) == nullptr);
    KILN_CHECK_EQ(v.decoded_size(), u64(64)); // 60 bytes padded to 16

    Vec<u8> dst(nullptr, Tag::Test);
    dst.resize(usize(v.decoded_size()));
    KILN_REQUIRE(decode_payload(v, v.encoded(), dst.span(), DecodeOptions{true}).ok());
    KILN_CHECK(std::memcmp(dst.data() + v.lods()[0].streamOffset[0], pos.data(), pos.size()) == 0);
    KILN_CHECK(check_indices(v, dst.span()).ok());
}

// Writes the sample files that kiln-info's CTest entries read (tests/CMakeLists.txt).
KILN_TEST(Mesh, WriteSampleFiles) {
    char const* dir = kiln::test::sample_dir();

    TestMesh m;

    Vec<u8> const raw = write_ok(m.desc());
    KILN_REQUIRE(!raw.empty());
    write_sample_file(dir, "sample_raw.mesh", raw.span());

    WriteOptions paddedOpt;
    paddedOpt.encodedPadding = 32;
    Vec<u8> const padded     = write_ok(m.desc(), paddedOpt);
    KILN_REQUIRE(!padded.empty());
    write_sample_file(dir, "sample_padded.mesh", padded.span());
}

// ---------------------------------------------------------------------------
// Payload compression: meshoptimizer's limits, wide strides, the decode allocator
// ---------------------------------------------------------------------------

namespace {

/// The first blob of `v` with codec `c` and element size `size`, or false.
bool find_blob(MeshView const& v, Codec c, u32 size, PayloadBlob* out) {
    for (u32 i = 0; i < v.blobs().size(); ++i)
        if (Codec(v.blobs()[i].codec) == c && v.blobs()[i].elementSize == size) {
            *out = v.blobs()[i];
            return true;
        }
    return false;
}

struct CountingAllocator {
    u32 allocs = 0;
    Allocator a{&CountingAllocator::alloc_fn, &CountingAllocator::free_fn, this};
    static void* alloc_fn(void* user, usize size, usize align, Tag tag) {
        ++static_cast<CountingAllocator*>(user)->allocs;
        return kiln::alloc(default_allocator(), size, align, tag);
    }
    static void free_fn(void*, void* ptr, usize size, usize align, Tag tag) {
        kiln::free(default_allocator(), ptr, size, align, tag);
    }
};

} // namespace

// meshoptimizer only asserts its size limits: a blob that breaks one fails validation in decode_blob() and in
// MeshView::open(), before any decoder runs.
KILN_TEST(Mesh, BlobElementLimits) {
    TestMesh m;
    Vec<u8> bytes      = write_ok(m.desc(), {.compression = cook::CompressionScheme::Meshopt});
    Result<MeshView> v = open_bytes(bytes);
    KILN_REQUIRE(v.ok());
    PayloadBlob good{};
    KILN_REQUIRE(find_blob(*v, Codec::None, 16, &good)); // hull LOD0 stream 1 (pattern data: stored as it is)
    Span<u8 const> const encoded = v->encoded().subspan(good.encodedOffset, good.encodedSize);

    struct Case {
        char const* what;
        u8 codec, filter;
        u16 elementSize;
        u32 decodedSize;
    };
    Case const cases[] = {
        {"Quat on 16-byte elements",      u8(Codec::MeshoptVertex), u8(Filter::MeshoptQuat), 16,  good.decodedSize},
        {"Oct on 16-byte elements",       u8(Codec::MeshoptVertex), u8(Filter::MeshoptOct),  16,  good.decodedSize},
        {"a 260-byte vertex",             u8(Codec::MeshoptVertex), u8(Filter::None),        260, 260             },
        {"a 6-byte vertex",               u8(Codec::MeshoptVertex), u8(Filter::None),        6,   12              },
        {"3-byte indices",                u8(Codec::MeshoptIndex),  u8(Filter::None),        3,   9               },
        {"indices that are no triangles", u8(Codec::MeshoptIndex),  u8(Filter::None),        2,   8               },
    };
    for (Case const& c : cases) {
        PayloadBlob bad = good;
        bad.codec       = c.codec;
        bad.filter      = c.filter;
        bad.elementSize = c.elementSize;
        bad.decodedSize = c.decodedSize;
        Vec<u8> dst(nullptr, Tag::Test);
        dst.resize(c.decodedSize);
        Status const st = decode_blob(bad, encoded, dst.span());
        KILN_CHECK_MSG(st.code == Code::ValidationFailed, "%s: %s", c.what, code_name(st.code));
    }

    // That blob record in the file as MeshoptVertex with filter Quat: open() refuses the file.
    Vec<u8> tampered(nullptr, Tag::Test);
    tampered.resize(bytes.size());
    std::memcpy(tampered.data(), bytes.data(), bytes.size());
    u8 record[sizeof(PayloadBlob)];
    std::memcpy(record, &good, sizeof record);
    usize at = 0;
    while (at + sizeof record <= tampered.size() &&
           std::memcmp(tampered.data() + at, record, sizeof record) != 0)
        at += 4;
    KILN_REQUIRE(at + sizeof record <= tampered.size());
    PayloadBlob bad = good;
    bad.codec       = u8(Codec::MeshoptVertex);
    bad.filter      = u8(Filter::MeshoptQuat);
    std::memcpy(tampered.data() + at, &bad, sizeof bad);
    DiagCapture cap;
    DiagSink sink       = cap.sink();
    Result<MeshView> rv = open_bytes(tampered, {}, &sink);
    KILN_CHECK(rv.code() == Code::ValidationFailed);
    KILN_CHECK_EQ(cap.code, u32(kDiagBlobEncoding));
}

// A stride meshopt cannot take (above 256) is encoded with Zstd instead, and decodes to the input.
KILN_TEST(Mesh, WideStrideFallsBackToZstd) {
    TestMesh m;
    m.layouts[1].strides[1] = 260;
    fill_pattern(m.streams[2][1], 9 * 260, 6);
    m.lods[2].streams[1] = cspan(m.streams[2][1]);
    for (cook::CompressionScheme scheme :
         {cook::CompressionScheme::Meshopt, cook::CompressionScheme::MeshoptZstd}) {
        Vec<u8> bytes      = write_ok(m.desc(), {.compression = scheme});
        Result<MeshView> v = open_bytes(bytes);
        KILN_REQUIRE(v.ok());
        PayloadBlob b{};
        KILN_CHECK(!find_blob(*v, Codec::MeshoptVertex, 260, &b));
        KILN_REQUIRE(find_blob(*v, Codec::Zstd, 260, &b) || find_blob(*v, Codec::None, 260, &b));
        Vec<u8> dst(nullptr, Tag::Test); // indices may come back rotated (MeshoptIndex): compare the stream
        dst.resize(usize(v->decoded_size()));
        KILN_REQUIRE(decode_payload(*v, v->encoded(), dst.span(), {.verifyChecksums = true}).ok());
        KILN_CHECK(std::memcmp(dst.data() + b.decodedOffset, m.streams[2][1].data(), 9 * 260) == 0);
    }
}

// Decoding takes its Zstd context and intermediates from DecodeOptions::alloc.
KILN_TEST(Mesh, DecodeUsesTheGivenAllocator) {
    TestMesh m;
    Vec<u8> bytes      = write_ok(m.desc(), {.compression = cook::CompressionScheme::MeshoptZstd});
    Result<MeshView> v = open_bytes(bytes);
    KILN_REQUIRE(v.ok());
    Vec<u8> dst(nullptr, Tag::Test);
    dst.resize(usize(v->decoded_size()));
    CountingAllocator counting;
    u64 const io0 = default_alloc_stats(Tag::Io).allocCount;
    KILN_REQUIRE(
        decode_payload(*v, v->encoded(), dst.span(), {.verifyChecksums = true, .alloc = &counting.a}).ok());
    KILN_CHECK(counting.allocs > 0);
    KILN_CHECK_EQ(default_alloc_stats(Tag::Io).allocCount,
                  io0 + counting.allocs); // all of it, and only through it
}
