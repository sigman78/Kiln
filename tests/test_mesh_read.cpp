// tests/test_mesh_read.cpp — reader-only .mesh tests; the shipping (KILN_BUILD_COOK=OFF) build runs them.
// No kiln/cook/ includes and no mesh::write(): file images are hand-built per docs/mesh-format-spec.md.
// Writer round trips live in test_mesh.cpp.
#include "kiln_test.h"

#include "kiln/containers.h"
#include "kiln/mesh.h"

#include <cstdint>
#include <cstring>

using namespace kiln;
using namespace kiln::mesh;

// Reader-side layout facts; mesh.h asserts the sizes too.
static_assert(sizeof(FileHeader) == 80);
static_assert(sizeof(SectionEntry) == 32);
static_assert(sizeof(Bounds) == 32);
static_assert(sizeof(ModelInfo) == 48);
static_assert(sizeof(VertexAttrib) == 12);
static_assert(sizeof(VertexLayout) == 160);
static_assert(sizeof(MeshPart) == 112);
static_assert(sizeof(MeshLod) == 48);
static_assert(sizeof(Submesh) == 48);
static_assert(sizeof(MaterialSlot) == 80);
static_assert(sizeof(TextureBinding) == 16);
static_assert(sizeof(Mount) == 48);
static_assert(sizeof(PayloadBlob) == 32);

static_assert(kMagic == fourcc("KMSH"));
static_assert(index_size(IndexType::U8) == 1u);
static_assert(index_size(IndexType::U16) == 2u);
static_assert(index_size(IndexType::U32) == 4u);
static_assert(is_allowed_blob_encoding(Codec::MeshoptIndex, Filter::None));
static_assert(!is_allowed_blob_encoding(Codec::None, Filter::MeshoptOct));
static_assert(is_allowed_blob_encoding(Codec::Zstd, Filter::ByteShuffle));

namespace {

// Hand-built file: one layout (stream 0 Position R32G32B32_SFLOAT, stream 1 TexCoord0 R32G32_SFLOAT),
// one part with one LOD (3 vertices, 3 U16 indices, one submesh), one material, and a raw payload
// with 3 blobs (stream 0, stream 1, indices). Optionally an MNTS section for the mount tests.

/// Appends `size` bytes (copied from `data`, or zero-filled if null) to `buf`,
/// 16-byte aligned, and records a SectionEntry describing the range.
u64 append_section(Vec<u8>& buf, Vec<SectionEntry>& secs, u32 id, void const* data, usize size, u32 count,
                   u32 stride) {
    usize padded = align_up(buf.size(), usize(16));
    if (padded > buf.size()) buf.resize(padded);
    usize offset = buf.size();
    buf.resize(offset + size);
    if (data && size) std::memcpy(buf.data() + offset, data, size);
    SectionEntry e{};
    e.id     = id;
    e.flags  = 0;
    e.offset = offset;
    e.size   = size;
    e.count  = count;
    e.stride = stride;
    secs.push_back(e);
    return offset;
}

// Canonical vertex data shared by the payload builder and the decode checks.
constexpr f32 kPos[3][3] = {
    {0, 0, 0},
    {1, 0, 0},
    {0, 1, 0}
};
constexpr f32 kUv[3][2] = {
    {0, 0},
    {1, 0},
    {0, 1}
};
constexpr u16 kIdx[3] = {0, 1, 2};

/// The GPUD payload: positions (stream 0, 3 * 12 B), UVs (stream 1, 3 * 8 B) and
/// indices (3 * U16), each range 16-byte aligned, raw (encoded == decoded).
struct Payload {
    Vec<u8> bytes{default_allocator(), Tag::Test};
    u32 streamOffset[2] = {0, 48}; // 36 B of positions, then the next 16-aligned slot
    u32 indexOffset     = 80;      // 24 B of UVs from 48, then the next 16-aligned slot
    PayloadBlob blobs[3]{};

    Payload() {
        bytes.resize(96); // rounds the index range [80, 86) up to a multiple of 16
        for (u32 i = 0; i < 3; ++i)
            std::memcpy(bytes.data() + streamOffset[0] + usize(i) * 12, kPos[i], 12);
        for (u32 i = 0; i < 3; ++i)
            std::memcpy(bytes.data() + streamOffset[1] + usize(i) * 8, kUv[i], 8);
        for (u32 i = 0; i < 3; ++i)
            write_unaligned(bytes.data() + indexOffset + usize(i) * 2, kIdx[i]);

        auto make_blob = [&](u32 off, u32 size, u16 elem) {
            PayloadBlob b{};
            b.encodedOffset = off;
            b.encodedSize   = size;
            b.decodedOffset = off;
            b.decodedSize   = size;
            b.elementSize   = elem;
            b.codec         = u8(Codec::None);
            b.filter        = u8(Filter::None);
            b.lodRank       = 0;
            b.checksum      = xxh32(bytes.data() + off, size);
            return b;
        };
        blobs[0] = make_blob(streamOffset[0], 36, 12);
        blobs[1] = make_blob(streamOffset[1], 24, 8);
        blobs[2] = make_blob(indexOffset, 6, 2);
    }
};

/// Builds the CPU-region metadata, then GPUD. `mounts`, when non-empty, becomes
/// an MNTS section (the caller keeps it sorted by nameHash).
Vec<u8> build_mesh(Span<Mount const> mounts = {}) {
    static u8 const kStrs[] = {0, 'c', 'u', 'b', 'e', 0, 'p', 'a', 'r', 't', '0', 0, 'm', 'a', 't', '0', 0};
    constexpr u32 kStrCube  = 1;
    constexpr u32 kStrPart0 = 6;
    constexpr u32 kStrMat0  = 12;

    Payload payload;

    ModelInfo model{};
    model.bounds = Bounds{
        {0, 0, 0},
        1.0f, {1, 1, 1},
        0
    };
    model.nameStr = kStrCube;
    model.assetId = hash_name("cube");

    VertexLayout layout{};
    layout.streamCount = 2;
    layout.attribCount = 2;
    layout.strides[0]  = 12;
    layout.strides[1]  = 8;
    layout.attribs[0]  = VertexAttrib{u8(Semantic::Position), 0, 0, 0, u32(Format::R32G32B32_SFLOAT), 0, 0};
    layout.attribs[1]  = VertexAttrib{u8(Semantic::TexCoord), 0, 1, 0, u32(Format::R32G32_SFLOAT), 0, 0};

    MeshPart part{};
    part.nameStr     = kStrPart0;
    part.parent      = kInvalid;
    part.nameHash    = hash_name("part0");
    part.rotation[3] = 1.0f;
    part.bounds      = Bounds{
             {0, 0, 0},
             1.0f, {1, 1, 1},
             0
    };
    part.posScale[0] = part.posScale[1] = part.posScale[2] = 1.0f;
    part.lodFirst                                          = 0;
    part.lodCount                                          = 1;

    MeshLod lod{};
    lod.layout          = 0;
    lod.vertexCount     = 3;
    lod.streamOffset[0] = payload.streamOffset[0];
    lod.streamOffset[1] = payload.streamOffset[1];
    lod.streamOffset[2] = kInvalid;
    lod.streamOffset[3] = kInvalid;
    lod.indexOffset     = payload.indexOffset;
    lod.indexCount      = 3;
    lod.indexType       = u8(IndexType::U16);
    lod.submeshFirst    = 0;
    lod.submeshCount    = 1;

    Submesh submesh{};
    submesh.indexCount = 3;
    submesh.bounds     = Bounds{
            {0, 0, 0},
            1.0f, {1, 1, 1},
            0
    };

    MaterialSlot material{};
    material.nameStr     = kStrMat0;
    material.nameHash    = hash_name("mat0");
    material.alphaMode   = u8(AlphaMode::Opaque);
    material.alphaCutoff = 0.5f;

    u32 const sectionCount = 9 + (mounts.empty() ? 0u : 1u);
    Vec<u8> buf(default_allocator(), Tag::Test);
    Vec<SectionEntry> secs(default_allocator(), Tag::Test);
    buf.resize(sizeof(FileHeader) + usize(sectionCount) * sizeof(SectionEntry));

    append_section(buf, secs, kSecModel, &model, sizeof model, 1, sizeof(ModelInfo));
    append_section(buf, secs, kSecStrings, kStrs, sizeof kStrs, 0, 0);
    append_section(buf, secs, kSecLayouts, &layout, sizeof layout, 1, sizeof(VertexLayout));
    append_section(buf, secs, kSecParts, &part, sizeof part, 1, sizeof(MeshPart));
    append_section(buf, secs, kSecLods, &lod, sizeof lod, 1, sizeof(MeshLod));
    append_section(buf, secs, kSecSubmeshes, &submesh, sizeof submesh, 1, sizeof(Submesh));
    append_section(buf, secs, kSecMaterials, &material, sizeof material, 1, sizeof(MaterialSlot));
    if (!mounts.empty())
        append_section(buf, secs, kSecMounts, mounts.data, mounts.size_bytes(), u32(mounts.size),
                       sizeof(Mount));
    append_section(buf, secs, kSecBlobs, payload.blobs, sizeof payload.blobs, 3, sizeof(PayloadBlob));

    u64 const gpuDataOffset = align_up(buf.size(), usize(kPayloadBaseAlign));
    if (usize(gpuDataOffset) > buf.size()) buf.resize(usize(gpuDataOffset));
    u64 const gpuOff =
        append_section(buf, secs, kSecGpuData, payload.bytes.data(), payload.bytes.size(), 0, 0);
    KILN_VERIFY(gpuOff == gpuDataOffset);

    FileHeader header{};
    header.magic              = kMagic;
    header.versionMajor       = kVersionMajor;
    header.versionMinor       = kVersionMinor;
    header.flags              = kPayloadRaw;
    header.sectionCount       = sectionCount;
    header.fileSize           = buf.size();
    header.sectionTableOffset = sizeof(FileHeader);
    header.sourceHash         = 0x1111111111111111ull;
    header.cookHash           = 0x2222222222222222ull;
    header.gpuDataOffset      = gpuDataOffset;
    header.gpuDataSize        = header.fileSize - gpuDataOffset;
    header.payloadDecodedSize = header.gpuDataSize; // raw: identical layouts
    header.payloadAlignment   = kPayloadBaseAlign;

    write_unaligned(buf.data(), header);
    for (u32 i = 0; i < secs.size(); ++i)
        write_unaligned(buf.data() + sizeof(FileHeader) + usize(i) * sizeof(SectionEntry), secs[i]);
    return buf;
}

/// Three mounts, sorted by nameHash, with no names (nameStr/extrasStr == kInvalid
/// is explicitly allowed by the format).
void make_mounts(Mount (&mounts)[3]) {
    u64 const hashes[3] = {100, 200, 300};
    for (u32 i = 0; i < 3; ++i) {
        mounts[i]             = Mount{};
        mounts[i].nameStr     = kInvalid;
        mounts[i].parentPart  = 0;
        mounts[i].nameHash    = hashes[i];
        mounts[i].rotation[3] = 1.0f;
        mounts[i].extrasStr   = kInvalid;
    }
}

/// Rewrites section `id` with `newStride` bytes per record (original bytes, then zeros) and
/// shifts every later section, GPUD and the header to match. Exercises the spec §4/§7 rule:
/// unknown trailing bytes zero-fill.
Vec<u8> widen_section_stride(Vec<u8> const& src, u32 id, u32 newStride) {
    FileHeader const h = read_unaligned<FileHeader>(src.data());
    u32 secIndex       = kInvalid;
    SectionEntry entry{};
    for (u32 i = 0; i < h.sectionCount; ++i) {
        SectionEntry const e =
            read_unaligned<SectionEntry>(src.data() + h.sectionTableOffset + usize(i) * sizeof(SectionEntry));
        if (e.id == id) {
            secIndex = i;
            entry    = e;
            break;
        }
    }
    KILN_VERIFY(secIndex != kInvalid);
    // GPUD must stay 256-byte aligned (spec §2), so its offset is realigned, not shifted by delta.
    u64 const delta          = u64(entry.count) * (newStride - entry.stride);
    u64 const shiftedGpuData = h.gpuDataOffset + delta;
    u64 const newGpuData     = align_up(shiftedGpuData, u64(kPayloadBaseAlign));
    u64 const newFileSize    = newGpuData + h.gpuDataSize;

    Vec<u8> out(default_allocator(), Tag::Test);
    out.resize(usize(newFileSize)); // zero-initialized

    usize const head = usize(entry.offset);
    std::memcpy(out.data(), src.data(), head);
    for (u32 i = 0; i < entry.count; ++i)
        std::memcpy(out.data() + head + usize(i) * newStride, src.data() + head + usize(i) * entry.stride,
                    entry.stride);
    // Metadata sections between the widened one and GPUD keep the raw shift (16-B
    // alignment only; already satisfied since delta is a multiple of 16).
    usize const oldTail = usize(entry.offset + entry.size);
    std::memcpy(out.data() + oldTail + usize(delta), src.data() + oldTail, usize(h.gpuDataOffset) - oldTail);
    // GPUD itself lands at the realigned offset.
    std::memcpy(out.data() + usize(newGpuData), src.data() + usize(h.gpuDataOffset), usize(h.gpuDataSize));

    FileHeader nh    = h;
    nh.fileSize      = newFileSize;
    nh.gpuDataOffset = newGpuData;
    write_unaligned(out.data(), nh);

    for (u32 i = 0; i < h.sectionCount; ++i) {
        usize const entryOff = usize(h.sectionTableOffset) + usize(i) * sizeof(SectionEntry);
        SectionEntry e       = read_unaligned<SectionEntry>(out.data() + entryOff);
        if (i == secIndex) {
            e.stride = newStride;
            e.size   = u64(entry.count) * newStride;
        } else if (e.id == kSecGpuData) {
            e.offset = newGpuData;
        } else if (e.offset > entry.offset) {
            e.offset += delta;
        }
        write_unaligned(out.data() + entryOff, e);
    }
    return out;
}

struct DiagCapture {
    u32 code          = 0;
    Severity severity = Severity::Info;
    u32 count         = 0;
    char message[256]{};

    DiagSink sink() { return DiagSink{&DiagCapture::on_diag, this}; }

    static void on_diag(void* user, Diagnostic const& d) {
        auto* self     = static_cast<DiagCapture*>(user);
        self->code     = d.code;
        self->severity = d.severity;
        ++self->count;
        usize n = min(d.message.size, sizeof(self->message) - 1);
        if (n) std::memcpy(self->message, d.message.data, n);
        self->message[n] = '\0';
    }
};

u64 section_offset(Vec<u8> const& b, u32 id) {
    FileHeader const h = read_unaligned<FileHeader>(b.data());
    for (u32 i = 0; i < h.sectionCount; ++i) {
        SectionEntry const e =
            read_unaligned<SectionEntry>(b.data() + h.sectionTableOffset + usize(i) * sizeof(SectionEntry));
        if (e.id == id) return e.offset;
    }
    return ~u64(0);
}
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

} // namespace

KILN_TEST(Mesh, HandBuiltOpensAndResolves) {
    Vec<u8> const file = build_mesh();
    auto r             = MeshView::open(file.span());
    KILN_REQUIRE(r.ok());
    MeshView const& v = *r;

    FileHeader const& h = v.header();
    KILN_CHECK_EQ(h.magic, kMagic);
    KILN_CHECK_EQ(h.versionMajor, kVersionMajor);
    KILN_CHECK_EQ(h.versionMinor, kVersionMinor);
    KILN_CHECK_EQ(h.flags, u32(kPayloadRaw));
    KILN_CHECK(v.payload_raw());
    KILN_CHECK_EQ(h.fileSize, u64(file.size()));
    KILN_CHECK_EQ(h.gpuDataSize, h.payloadDecodedSize);
    KILN_CHECK_EQ(v.decoded_size(), u64(96));

    u32 const expected[] = {kSecModel,     kSecStrings,   kSecLayouts, kSecParts,  kSecLods,
                            kSecSubmeshes, kSecMaterials, kSecBlobs,   kSecGpuData};
    KILN_REQUIRE_EQ(v.sections().size, countof(expected));
    for (usize i = 0; i < countof(expected); ++i)
        KILN_CHECK_EQ(v.sections()[i].id, expected[i]);

    KILN_CHECK_EQ(v.name(), StrView("cube"));
    KILN_CHECK_EQ(v.asset_id(), hash_name("cube"));
    KILN_CHECK_EQ(v.str(0), StrView(""));

    KILN_REQUIRE_EQ(v.layouts().size(), 1u);
    KILN_CHECK_EQ(v.layouts()[0].strides[0], u16(12));
    KILN_CHECK_EQ(v.layouts()[0].strides[1], u16(8));

    KILN_REQUIRE_EQ(v.parts().size(), 1u);
    KILN_CHECK_EQ(v.str(v.parts()[0].nameStr), StrView("part0"));
    KILN_CHECK(v.find_part(hash_name("part0")) == &v.parts()[0]);
    KILN_CHECK(v.find_part(hash_name("nope")) == nullptr);

    KILN_REQUIRE_EQ(v.lods().size(), 1u);
    MeshLod const& lod = v.lods()[0];
    KILN_CHECK_EQ(lod.layout, 0u);
    KILN_CHECK_EQ(lod.vertexCount, 3u);
    KILN_CHECK_EQ(lod.streamOffset[0], 0u);
    KILN_CHECK_EQ(lod.streamOffset[1], 48u);
    KILN_CHECK_EQ(lod.streamOffset[2], kInvalid);
    KILN_CHECK_EQ(lod.streamOffset[3], kInvalid);
    KILN_CHECK_EQ(lod.indexOffset, 80u);
    KILN_CHECK_EQ(lod.indexCount, 3u);
    KILN_CHECK_EQ(lod.indexType, u8(IndexType::U16));
    KILN_CHECK_EQ(v.stream_bytes(lod, 0), u64(36));
    KILN_CHECK_EQ(v.stream_bytes(lod, 1), u64(24));
    KILN_CHECK_EQ(MeshView::index_bytes(lod), u64(6));

    KILN_REQUIRE_EQ(v.submeshes().size(), 1u);
    KILN_CHECK_EQ(v.submeshes()[0].material, 0u);
    KILN_CHECK_EQ(v.submeshes()[0].indexCount, 3u);

    KILN_REQUIRE_EQ(v.materials().size(), 1u);
    KILN_CHECK_EQ(v.str(v.materials()[0].nameStr), StrView("mat0"));
    KILN_CHECK(v.textures().empty());
    KILN_CHECK(v.mounts().empty());
    KILN_CHECK_EQ(v.blobs().size(), 3u);
}

KILN_TEST(Mesh, HandBuiltFindMount) {
    Mount mounts[3];
    make_mounts(mounts);
    Vec<u8> const file = build_mesh(Span<Mount const>(mounts, 3));
    auto r             = MeshView::open(file.span());
    KILN_REQUIRE(r.ok());
    MeshView const& v = *r;

    KILN_REQUIRE_EQ(v.mounts().size(), 3u);
    KILN_CHECK(v.find_mount(100) == &v.mounts()[0]);
    KILN_CHECK(v.find_mount(200) == &v.mounts()[1]);
    KILN_CHECK(v.find_mount(300) == &v.mounts()[2]);
    KILN_CHECK(v.find_mount(150) == nullptr);
}

KILN_TEST(Mesh, HandBuiltRecordsZeroFillNewFields) {
    Vec<u8> const good    = build_mesh();
    Vec<u8> const widened = widen_section_stride(good, kSecLods, 64);
    auto r                = MeshView::open(widened.span());
    KILN_REQUIRE(r.ok());
    MeshView const& v = *r;

    KILN_CHECK_EQ(v.lods().stride(), 64u);
    KILN_CHECK(!v.lods().contiguous());
    KILN_CHECK_EQ(v.lods()[0].vertexCount, 3u);
    KILN_CHECK_EQ(v.lods().get(0).vertexCount, 3u);
}

// decode_payload / check_indices on the hand-built raw file.
KILN_TEST(Mesh, HandBuiltDecodeReproducesInput) {
    Vec<u8> const file = build_mesh();
    auto r             = MeshView::open(file.span());
    KILN_REQUIRE(r.ok());
    MeshView const& v = *r;

    Vec<u8> dst(default_allocator(), Tag::Test);
    dst.resize(usize(v.decoded_size()));
    DiagCapture cap;
    DiagSink sink = cap.sink();
    Status st     = decode_payload(v, v.encoded(), dst.span(), DecodeOptions{true}, &sink);
    KILN_CHECK_MSG(st.ok(), "decode_payload: %s", cap.message);
    if (st.failed()) return;
    KILN_CHECK(std::memcmp(dst.data(), v.encoded().data, dst.size()) == 0);

    MeshLod const& lod = v.lods()[0];
    for (u32 i = 0; i < 3; ++i)
        KILN_CHECK(std::memcmp(dst.data() + lod.streamOffset[0] + usize(i) * 12, kPos[i], 12) == 0);
    for (u32 i = 0; i < 3; ++i)
        KILN_CHECK(std::memcmp(dst.data() + lod.streamOffset[1] + usize(i) * 8, kUv[i], 8) == 0);
    for (u32 i = 0; i < 3; ++i)
        KILN_CHECK_EQ(read_unaligned<u16>(dst.data() + lod.indexOffset + usize(i) * 2), kIdx[i]);

    KILN_CHECK(check_indices(v, dst.span()).ok());
}

KILN_TEST(Mesh, HandBuiltMisalignedBufferRejected) {
    Vec<u8> const good = build_mesh();
    Vec<u8> big(default_allocator(), Tag::Test);
    big.resize(good.size() + 16);
    auto addr = reinterpret_cast<std::uintptr_t>(big.data());
    usize off = usize((8 - (addr % 8)) % 8) +
                4; // big.data() + off is misaligned by 4, whatever big's own alignment is
    std::memcpy(big.data() + off, good.data(), good.size());

    DiagCapture cap;
    DiagSink sink = cap.sink();
    auto r        = MeshView::open(Span<u8 const>(big.data() + off, good.size()), {}, &sink);
    KILN_CHECK(r.failed());
    KILN_CHECK_EQ(r.code(), Code::InvalidArgument);
    KILN_CHECK_EQ(cap.code, u32(kDiagBufferAlignment));
}

KILN_TEST(Mesh, HandBuiltCorruptionsDetected) {
    Vec<u8> const good = build_mesh();
    KILN_REQUIRE(MeshView::open(good.span()).ok());

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
        // Shorter than sizeof(FileHeader).
        expect_open_fails(good, Code::Corrupt, kDiagTruncated, "shorter than header", {}, usize(40));
    }
    {
        // Shorter than gpuDataOffset (truncated CPU region).
        u64 gpuOff = read_unaligned<FileHeader>(good.data()).gpuDataOffset;
        expect_open_fails(good, Code::Corrupt, kDiagTruncated, "truncated CPU region", {},
                          usize(gpuOff - 16));
    }
    {
        Vec<u8> b = good.clone();
        patch_header(b, [](FileHeader& h) { h.gpuDataSize += 16; });
        expect_open_fails(b, Code::Corrupt, kDiagHeaderSizes, "gpuDataSize mismatch");
    }
    {
        // Pull the stream-1 blob back into the stream-0 blob: unsorted / overlapping table.
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
        // A blob shorter than the stream it holds: no blob covers the stream's last element.
        Vec<u8> b = good.clone();
        patch_record<PayloadBlob>(b, kSecBlobs, 0, [](PayloadBlob& p) {
            p.encodedSize -= p.elementSize;
            p.decodedSize -= p.elementSize;
        });
        expect_open_fails(b, Code::Corrupt, kDiagLodRange, "stream not covered by blobs");
    }
    {
        // The same for a blob that is not the first of the table.
        Vec<u8> b = good.clone();
        patch_record<PayloadBlob>(b, kSecBlobs, 1, [](PayloadBlob& p) {
            p.encodedSize -= p.elementSize;
            p.decodedSize -= p.elementSize;
        });
        expect_open_fails(b, Code::Corrupt, kDiagLodRange, "second stream not covered by blobs");
    }
    {
        Vec<u8> b = good.clone();
        patch_record<PayloadBlob>(b, kSecBlobs, 0, [](PayloadBlob& p) { p.filter = u8(Filter::MeshoptOct); });
        expect_open_fails(b, Code::ValidationFailed, kDiagBlobEncoding, "codec None + MeshoptOct");
    }
    {
        // kPayloadRaw is set; breaking one blob's identity conditions must be caught.
        Vec<u8> b = good.clone();
        patch_record<PayloadBlob>(b, kSecBlobs, 0, [](PayloadBlob& p) { p.codec = u8(Codec::Zstd); });
        expect_open_fails(b, Code::Corrupt, kDiagPayloadRaw, "kPayloadRaw violated");
    }
    {
        Vec<u8> b = good.clone();
        auto r    = MeshView::open(good.span());
        KILN_REQUIRE(r.ok());
        SectionEntry const* s = r->find_section(kSecStrings);
        KILN_REQUIRE(s != nullptr);
        b[usize(s->offset + s->size - 1)] = u8('x');
        expect_open_fails(b, Code::Corrupt, kDiagStringOffset, "STRS without final NUL");
    }
    {
        Vec<u8> b = good.clone();
        patch_record<MeshPart>(b, kSecParts, 0, [](MeshPart& p) { p.parent = 0; });
        expect_open_fails(b, Code::ValidationFailed, kDiagPartOrder, "part parent = self");
        // With validation off only the structural (always-on) checks run.
        OpenOptions noValidate;
        noValidate.validate = false;
        KILN_CHECK(MeshView::open(b.span(), noValidate).ok());
    }
    {
        Mount mounts[3];
        make_mounts(mounts);
        Vec<u8> b = build_mesh(Span<Mount const>(mounts, 3));
        patch_record<Mount>(b, kSecMounts, 0, [](Mount& m) { m.nameHash = 250; }); // 250, 200, 300: unsorted
        expect_open_fails(b, Code::ValidationFailed, kDiagMountOrder, "mounts not sorted");
    }
}
