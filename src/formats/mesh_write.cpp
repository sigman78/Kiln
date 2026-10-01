// .mesh writer. Spec: docs/mesh-format-spec.md v0.3 (§2 invariants, §4 header, §5.9 BLOB).
// For determinism the image starts zeroed and every record starts as a value-initialized
// local, so _pad/_reserved fields are always 0.
#include "kiln/cook/mesh_writer.h"

#include "formats_internal.h"

#include "meshoptimizer.h"

#define ZSTD_STATIC_LINKING_ONLY // the custom-allocator API (the vendored zstd is pinned)
#include <zstd.h>

#include <algorithm>

namespace kiln::mesh {

namespace {

constexpr u64 kLimit32 = u64(1) << 32;

u64 align16(u64 v) noexcept { return align_up(v, u64(kBlobAlign)); }

#define KILN_WRITE_FAIL(diagCode, ...)                                                                       \
    return diagf(diag, make_status(Code::InvalidArgument), diagCode, Severity::Error, d.name, StrView{},     \
                 __VA_ARGS__)

// ---------------------------------------------------------------------------
// Input validation
// ---------------------------------------------------------------------------

/// Same rules as the reader's validate_layouts (spec §5.3).
Status validate_layout(WriteDesc const& d, DiagSink const* diag, u32 li, VertexLayout const& l) noexcept {
    if (l.streamCount < 1 || l.streamCount > kMaxStreams || l.attribCount < 1 || l.attribCount > kMaxAttribs)
        KILN_WRITE_FAIL(kDiagLayoutRule, "layout %u: streamCount %u / attribCount %u", li,
                        unsigned(l.streamCount), unsigned(l.attribCount));
    for (u32 s = 0; s < l.streamCount; ++s)
        if (l.strides[s] == 0 || (l.strides[s] % 4) != 0)
            KILN_WRITE_FAIL(kDiagLayoutRule, "layout %u: stream %u stride %u", li, s, unsigned(l.strides[s]));
    u32 stream0Attribs = 0;
    for (u32 a = 0; a < l.attribCount; ++a) {
        VertexAttrib const& at = l.attribs[a];
        if (at.stream >= l.streamCount)
            KILN_WRITE_FAIL(kDiagLayoutRule, "layout %u attrib %u: stream %u out of range", li, a,
                            unsigned(at.stream));
        if (at.semantic > u8(Semantic::Custom))
            KILN_WRITE_FAIL(kDiagLayoutRule, "layout %u attrib %u: semantic %u", li, a,
                            unsigned(at.semantic));
        FormatInfo const* fi = format_info(Format(at.format));
        if (!fi || fi->compressed)
            KILN_WRITE_FAIL(kDiagLayoutRule, "layout %u attrib %u: format %u not a vertex format", li, a,
                            at.format);
        if (u32(at.offset) + fi->bytesPerBlock > l.strides[at.stream])
            KILN_WRITE_FAIL(kDiagLayoutRule, "layout %u attrib %u: offset %u + %u > stride %u", li, a,
                            unsigned(at.offset), unsigned(fi->bytesPerBlock), unsigned(l.strides[at.stream]));
        if (at.stream == 0) {
            ++stream0Attribs;
            if (at.semantic != u8(Semantic::Position))
                KILN_WRITE_FAIL(kDiagLayoutRule, "layout %u: stream 0 must contain only Position", li);
        }
    }
    if (stream0Attribs != 1)
        KILN_WRITE_FAIL(kDiagLayoutRule, "layout %u: stream 0 must hold exactly one attribute", li);
    return kOk;
}

Status validate(WriteDesc const& d, WriteOptions const& opt, DiagSink const* diag) noexcept {
    if (opt.payloadAlignment < kPayloadBaseAlign || !is_pow2(opt.payloadAlignment))
        KILN_WRITE_FAIL(kDiagHeaderSizes, "payloadAlignment %u must be a power of two >= %u",
                        opt.payloadAlignment, kPayloadBaseAlign);

    usize const counts[] = {d.layouts.size,   d.parts.size,    d.lods.size,  d.submeshes.size,
                            d.materials.size, d.textures.size, d.mounts.size};
    for (usize c : counts)
        if (c >= kInvalid) KILN_WRITE_FAIL(kDiagIndexRange, "record count exceeds the u32 range");

    if (d.layouts.empty()) KILN_WRITE_FAIL(kDiagSectionMissing, "no vertex layouts");
    if (d.parts.empty()) KILN_WRITE_FAIL(kDiagSectionMissing, "no parts");

    u32 const nLayouts = u32(d.layouts.size), nParts = u32(d.parts.size), nLods = u32(d.lods.size);
    u32 const nSubm = u32(d.submeshes.size), nMats = u32(d.materials.size), nTex = u32(d.textures.size);

    for (u32 i = 0; i < nLayouts; ++i)
        KILN_TRY(validate_layout(d, diag, i, d.layouts[i]));

    for (u32 i = 0; i < nParts; ++i) {
        PartDesc const& p = d.parts[i];
        if (p.parent != kInvalid && p.parent >= i)
            KILN_WRITE_FAIL(kDiagPartOrder, "part %u: parent %u not before it", i, p.parent);
        if (p.lodCount != 0 && u64(p.lodFirst) + p.lodCount > nLods)
            KILN_WRITE_FAIL(kDiagIndexRange, "part %u: lods [%u, +%u) out of range (%u lods)", i, p.lodFirst,
                            p.lodCount, nLods);
    }

    for (u32 i = 0; i < nLods; ++i) {
        LodDesc const& l = d.lods[i];
        if (l.layout >= nLayouts)
            KILN_WRITE_FAIL(kDiagIndexRange, "lod %u: layout %u out of range", i, l.layout);
        VertexLayout const& lay = d.layouts[l.layout];
        for (u32 s = 0; s < kMaxStreams; ++s) {
            u64 want = s < lay.streamCount ? u64(l.vertexCount) * lay.strides[s] : 0;
            if (l.streams[s].size != want)
                KILN_WRITE_FAIL(kDiagLodRange, "lod %u: stream %u has %llu bytes, expected %llu", i, s,
                                static_cast<unsigned long long>(l.streams[s].size),
                                static_cast<unsigned long long>(want));
            if (want && !l.streams[s].data)
                KILN_WRITE_FAIL(kDiagLodRange, "lod %u: stream %u data is null", i, s);
        }
        if (u8(l.indexType) > u8(IndexType::U8))
            KILN_WRITE_FAIL(kDiagLodRange, "lod %u: indexType %u", i, unsigned(l.indexType));
        u64 ibytes = u64(l.indexCount) * index_size(l.indexType);
        if (l.indices.size != ibytes)
            KILN_WRITE_FAIL(kDiagLodRange, "lod %u: index buffer has %llu bytes, expected %llu", i,
                            static_cast<unsigned long long>(l.indices.size),
                            static_cast<unsigned long long>(ibytes));
        if (ibytes && !l.indices.data) KILN_WRITE_FAIL(kDiagLodRange, "lod %u: index data is null", i);
        if (u64(l.submeshFirst) + l.submeshCount > nSubm)
            KILN_WRITE_FAIL(kDiagIndexRange, "lod %u: submeshes [%u, +%u) out of range", i, l.submeshFirst,
                            l.submeshCount);
        for (u32 s = 0; s < l.submeshCount; ++s) {
            Submesh const& sm = d.submeshes[l.submeshFirst + s];
            if (sm.material >= nMats)
                KILN_WRITE_FAIL(kDiagIndexRange, "lod %u submesh %u: material %u out of range", i, s,
                                sm.material);
            if (u64(sm.indexFirst) + sm.indexCount > l.indexCount)
                KILN_WRITE_FAIL(kDiagIndexRange, "lod %u submesh %u: indices [%u, +%u) exceed the lod's %u",
                                i, s, sm.indexFirst, sm.indexCount, l.indexCount);
        }
    }

    for (u32 i = 0; i < nMats; ++i) {
        MaterialDesc const& m = d.materials[i];
        if (u8(m.alphaMode) > u8(AlphaMode::Blend))
            KILN_WRITE_FAIL(kDiagIndexRange, "material %u: alphaMode %u", i, unsigned(m.alphaMode));
        if (u64(m.textureFirst) + m.textureCount > nTex)
            KILN_WRITE_FAIL(kDiagIndexRange, "material %u: textures [%u, +%u) out of range", i,
                            m.textureFirst, m.textureCount);
    }
    for (u32 i = 0; i < nTex; ++i)
        if (u8(d.textures[i].slot) > 15)
            KILN_WRITE_FAIL(kDiagIndexRange, "texture %u: slot %u", i, unsigned(d.textures[i].slot));
    for (u32 i = 0; i < u32(d.mounts.size); ++i)
        if (d.mounts[i].parentPart != kInvalid && d.mounts[i].parentPart >= nParts)
            KILN_WRITE_FAIL(kDiagIndexRange, "mount %u: parentPart %u out of range", i,
                            d.mounts[i].parentPart);
    return kOk;
}

// Builds the STRS section.
class StringTable {
public:
    explicit StringTable(Allocator const* a) noexcept : bytes_(a, Tag::Cook), map_(a, Tag::Cook) {
        (void)intern(StrView{}); // offset 0 is the empty string
    }
    u32 intern(StrView s) {
        auto r = map_.try_emplace(s, u32(bytes_.size()));
        if (r.inserted) {
            bytes_.append(Span<u8 const>(reinterpret_cast<u8 const*>(s.data), s.size));
            bytes_.push_back(0);
        }
        return *r.value;
    }
    /// Zero-pad to a multiple of 16 (the padding is NULs, so STRS still ends with one).
    void finish() {
        while (bytes_.size() % kBlobAlign)
            bytes_.push_back(0);
    }
    Vec<u8> const& bytes() const noexcept { return bytes_; }

private:
    Vec<u8> bytes_;
    HashMap<StrView, u32> map_;
};

struct BlobOut {
    PayloadBlob rec;
    u8 const* src;   ///< decoded bytes
    Vec<u8> encoded; ///< empty: codec None, the encoded bytes are `src`
    bool index = false;
    Vec<u8> rotated; ///< MeshoptIndex: the indices as they decode; `src` then points here
};

/// One blob's encoding under a scheme: meshopt for vertices and triangle indices, else Zstd.
struct Encoder {
    ZSTD_CCtx* cctx = nullptr;
    Vec<u8> shuffled;
    Vec<u8> inner;

    /// Zstd of `in` into `out`; false when it fails.
    bool zstd(Span<u8 const> in, Vec<u8>& out) noexcept {
        out.resize(ZSTD_compressBound(in.size));
        usize const n = ZSTD_compress2(cctx, out.data(), out.size(), in.data, in.size);
        if (ZSTD_isError(n)) return false;
        out.resize(n);
        return true;
    }

    /// Fills `b.encoded` and the codec fields; leaves the blob None when nothing shrinks it.
    bool encode(BlobOut& b, cook::CompressionScheme scheme) noexcept {
        bool const index  = b.index;
        usize const n     = b.rec.decodedSize;
        usize const size  = b.rec.elementSize;
        usize const count = n / size;
        Span<u8 const> const src(b.src, n);
        bool const meshopt =
            scheme == cook::CompressionScheme::Meshopt || scheme == cook::CompressionScheme::MeshoptZstd;
        Codec codec   = Codec::Zstd;
        Filter filter = Filter::None;
        u16 flags     = 0;
        // meshopt only where its limits allow (a stride above 256 takes the Zstd path).
        if (meshopt && !index && !blob_element_problem(Codec::MeshoptVertex, Filter::None, u32(size))) {
            inner.resize(meshopt_encodeVertexBufferBound(count, size));
            usize const m =
                meshopt_encodeVertexBufferLevel(inner.data(), inner.size(), b.src, count, size, 2, 1);
            if (m == 0) return false;
            inner.resize(m);
            codec = Codec::MeshoptVertex;
        } else if (meshopt && index && !blob_element_problem(Codec::MeshoptIndex, Filter::None, u32(size)) &&
                   count % 3 == 0) {
            inner.resize(meshopt_encodeIndexBufferBound(count, size == 2 ? usize(1) << 16 : usize(1) << 31));
            usize const m = size == 2 ? meshopt_encodeIndexBuffer(inner.data(), inner.size(),
                                                                  reinterpret_cast<u16 const*>(b.src), count)
                                      : meshopt_encodeIndexBuffer(inner.data(), inner.size(),
                                                                  reinterpret_cast<u32 const*>(b.src), count);
            if (m == 0) return false;
            inner.resize(m);
            codec = Codec::MeshoptIndex;
            // The codec may rotate a triangle's vertices (winding and triangle order stay): the payload
            // is what it decodes to, so the checksum and every reader agree.
            b.rotated.resize(n);
            if (meshopt_decodeIndexBuffer(b.rotated.data(), count, size, inner.data(), inner.size()) != 0)
                return false;
        }
        if (codec == Codec::Zstd) { // what meshopt cannot take
            Span<u8 const> in = src;
            if (!index && size > 1) {
                shuffled.resize(n);
                for (usize byte = 0; byte < size; ++byte)
                    for (usize i = 0; i < count; ++i)
                        shuffled[byte * count + i] = b.src[i * size + byte];
                in     = shuffled.span();
                filter = Filter::ByteShuffle;
            }
            if (!zstd(in, b.encoded)) return false;
        } else if (scheme == cook::CompressionScheme::MeshoptZstd) {
            if (!zstd(inner.span(), b.encoded)) return false;
            flags = kBlobOuterZstd;
        } else {
            b.encoded.resize(inner.size());
            std::memcpy(b.encoded.data(), inner.data(), inner.size());
        }
        if (b.encoded.size() >= n) { // no gain: stored as it is
            b.encoded.clear();
            return true;
        }
        b.rec.codec  = u8(codec);
        b.rec.filter = u8(filter);
        b.rec.flags  = flags;
        if (codec == Codec::MeshoptIndex) {
            b.src = b.rotated.data();
            if (b.rec.checksum != 0) b.rec.checksum = xxh32(b.src, n);
        }
        return true;
    }
};

struct SectionOut {
    u32 id;
    void const* data;
    u64 bytes; ///< bytes to copy == SectionEntry.size
    u32 count;
    u32 stride;
    u64 offset;
};

void clear_bounds_pad(Bounds& b) noexcept { b._pad = 0; }

} // namespace

Result<Vec<u8>> write(WriteDesc const& desc, WriteOptions const& opt, Allocator const* alloc,
                      DiagSink const* diag, WriteStats* stats) noexcept {
    WriteDesc const& d = desc; // for KILN_WRITE_FAIL
    if (!alloc) alloc = default_allocator();
    KILN_TRY(validate(d, opt, diag));

    u32 const nParts = u32(d.parts.size), nLods = u32(d.lods.size), nMounts = u32(d.mounts.size);
    u64 const encPad = align16(opt.encodedPadding);

    // -- lodRank per LOD (spec §5.9) -----------------------------------------
    Vec<u8> rank(alloc, Tag::Cook), owned(alloc, Tag::Cook);
    rank.resize(nLods, u8(255));
    owned.resize(nLods, u8(0));
    for (PartDesc const& p : d.parts) {
        for (u32 j = 0; j < p.lodCount; ++j) {
            u32 li = p.lodFirst + j;
            if (owned[li]) continue;
            owned[li] = 1;
            rank[li]  = u8(min(p.lodCount - 1 - j, 255u));
        }
    }
    for (u32 li = 0; li < nLods; ++li)
        if (!owned[li])
            (void)diagf(diag, kOk, kDiagIndexRange, Severity::Warning, d.name, StrView{},
                        "lod %u not referenced by any part", li);

    // Blob order: ascending lodRank, ties in LOD-record order.
    Vec<u32> order(alloc, Tag::Cook);
    order.resize(nLods);
    for (u32 i = 0; i < nLods; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](u32 a, u32 b) { return rank[a] != rank[b] ? rank[a] < rank[b] : a < b; });

    // -- decoded payload layout + blobs ----------------------------------------
    Vec<MeshLod> lodRecs(alloc, Tag::Cook);
    lodRecs.resize(nLods); // zeroed
    Vec<BlobOut> blobs(alloc, Tag::Cook);
    u64 cursor = 0;

    // One blob per stream per LOD and one per index range. Splitting ranges into
    // smaller blobs is deferred to the codec work (spec §5.9).
    auto emit_range = [&](u8 const* src, u64 bytes, u32 elementSize, u8 lodRank, bool index) {
        PayloadBlob b{};
        b.decodedOffset = u32(cursor);
        b.decodedSize   = u32(bytes);
        b.elementSize   = u16(elementSize);
        b.codec         = u8(Codec::None);
        b.filter        = u8(Filter::None);
        b.flags         = 0;
        b.lodRank       = lodRank;
        b.checksum      = opt.checksums ? xxh32(src, usize(bytes)) : 0u;
        blobs.push_back(BlobOut{b, src, Vec<u8>(alloc, Tag::Cook), index, Vec<u8>(alloc, Tag::Cook)});
    };

    for (u32 li : order) {
        LodDesc const& l        = d.lods[li];
        VertexLayout const& lay = d.layouts[l.layout];
        MeshLod& r              = lodRecs[li];
        r.layout                = l.layout;
        r.vertexCount           = l.vertexCount;
        for (u32 s = 0; s < kMaxStreams; ++s) {
            if (s >= lay.streamCount) {
                r.streamOffset[s] = kInvalid;
                continue;
            }
            u64 bytes = l.streams[s].size;
            cursor    = align16(cursor);
            if (cursor + bytes >= kLimit32)
                KILN_WRITE_FAIL(kDiagHeaderSizes,
                                "decoded payload exceeds the 4 GiB limit (lod %u stream %u)", li, s);
            r.streamOffset[s] = u32(cursor);
            emit_range(l.streams[s].data, bytes, lay.strides[s], rank[li], false);
            cursor += bytes;
        }
        u32 isz    = index_size(l.indexType);
        u64 ibytes = l.indices.size;
        cursor     = align16(cursor);
        if (cursor + ibytes >= kLimit32)
            KILN_WRITE_FAIL(kDiagHeaderSizes, "decoded payload exceeds the 4 GiB limit (lod %u indices)", li);
        r.indexOffset = u32(cursor);
        if (ibytes) emit_range(l.indices.data, ibytes, isz, rank[li], true);
        cursor += ibytes;
        r.indexCount     = l.indexCount;
        r.indexType      = u8(l.indexType);
        r.submeshFirst   = l.submeshFirst;
        r.submeshCount   = l.submeshCount;
        r.geometricError = l.geometricError;
    }
    u64 const decodedSize = align16(cursor);
    if (decodedSize >= kLimit32) KILN_WRITE_FAIL(kDiagHeaderSizes, "decoded payload exceeds the 4 GiB limit");

    // -- compression ---------------------------------------------------------------
    if (opt.compression != cook::CompressionScheme::None) {
        fmt::ZstdMem mem{alloc, Tag::Cook};
        Encoder e{ZSTD_createCCtx_advanced(ZSTD_customMem{&fmt::zstd_alloc, &fmt::zstd_free, &mem}),
                  Vec<u8>(alloc, Tag::Cook), Vec<u8>(alloc, Tag::Cook)};
        if (!e.cctx) KILN_WRITE_FAIL(kDiagHeaderSizes, "out of memory for the Zstd context");
        (void)ZSTD_CCtx_setParameter(e.cctx, ZSTD_c_compressionLevel, int(clamp<u8>(opt.zstdLevel, 1, 19)));
        (void)ZSTD_CCtx_setParameter(e.cctx, ZSTD_c_contentSizeFlag, 1);
        (void)ZSTD_CCtx_setParameter(e.cctx, ZSTD_c_checksumFlag, 0);
        bool ok = true;
        for (u32 i = 0; i < u32(blobs.size()) && ok; ++i)
            ok = e.encode(blobs[i], opt.compression);
        ZSTD_freeCCtx(e.cctx);
        if (!ok) KILN_WRITE_FAIL(kDiagHeaderSizes, "payload compression failed");
    }

    // -- encoded layout ----------------------------------------------------------
    u64 enc  = 0;
    bool raw = encPad == 0;
    for (BlobOut& b : blobs) {
        u64 const bytes = b.encoded.empty() ? b.rec.decodedSize : b.encoded.size();
        u64 eo          = align16(enc + encPad);
        if (eo + bytes >= kLimit32)
            KILN_WRITE_FAIL(kDiagHeaderSizes, "encoded payload exceeds the 4 GiB limit");
        b.rec.encodedOffset = u32(eo);
        b.rec.encodedSize   = u32(bytes);
        enc                 = eo + bytes;
        if (b.rec.encodedOffset != b.rec.decodedOffset || !b.encoded.empty()) raw = false;
    }
    u64 const gpuDataSize = align16(enc);
    if (gpuDataSize >= kLimit32) KILN_WRITE_FAIL(kDiagHeaderSizes, "encoded payload exceeds the 4 GiB limit");
    if (gpuDataSize != decodedSize) raw = false;

    // -- mounts: sort by (nameHash, input index) ---------------------------------
    Vec<u32> mountOrder(alloc, Tag::Cook);
    Vec<u64> mountHash(alloc, Tag::Cook);
    mountOrder.resize(nMounts);
    mountHash.resize(nMounts);
    for (u32 i = 0; i < nMounts; ++i) {
        mountOrder[i] = i;
        mountHash[i]  = hash_name(d.mounts[i].name);
    }
    std::sort(mountOrder.begin(), mountOrder.end(), [&](u32 a, u32 b) {
        return mountHash[a] != mountHash[b] ? mountHash[a] < mountHash[b] : a < b;
    });

    // -- strings (first-seen order) -----------------------------------------------
    StringTable strs(alloc);
    ModelInfo model{};
    model.bounds = d.bounds;
    clear_bounds_pad(model.bounds);
    model.nameStr = strs.intern(d.name);
    model.flags   = d.modelFlags;
    model.assetId = d.assetId ? d.assetId : hash_name(d.name);

    Vec<MeshPart> partRecs(alloc, Tag::Cook);
    partRecs.resize(nParts);
    for (u32 i = 0; i < nParts; ++i) {
        PartDesc const& p = d.parts[i];
        MeshPart& r       = partRecs[i];
        r.nameStr         = strs.intern(p.name);
        r.parent          = p.parent;
        r.nameHash        = hash_name(p.name);
        std::memcpy(r.translation, p.translation, sizeof r.translation);
        std::memcpy(r.rotation, p.rotation, sizeof r.rotation);
        r.bounds = p.bounds;
        clear_bounds_pad(r.bounds);
        std::memcpy(r.posScale, p.posScale, sizeof r.posScale);
        std::memcpy(r.posBias, p.posBias, sizeof r.posBias);
        r.lodFirst = p.lodFirst;
        r.lodCount = p.lodCount;
        r.flags    = p.flags;
    }

    Vec<MaterialSlot> matRecs(alloc, Tag::Cook);
    matRecs.resize(d.materials.size);
    for (usize i = 0; i < d.materials.size; ++i) {
        MaterialDesc const& m = d.materials[i];
        MaterialSlot& r       = matRecs[i];
        r.nameStr             = strs.intern(m.name);
        r.flags               = m.flags;
        r.nameHash            = hash_name(m.name);
        r.textureFirst        = m.textureFirst;
        r.textureCount        = m.textureCount;
        r.alphaMode           = u8(m.alphaMode);
        r.alphaCutoff         = m.alphaCutoff;
        std::memcpy(r.baseColorFactor, m.baseColorFactor, sizeof r.baseColorFactor);
        std::memcpy(r.emissiveFactor, m.emissiveFactor, sizeof r.emissiveFactor);
        r.metallicFactor    = m.metallicFactor;
        r.roughnessFactor   = m.roughnessFactor;
        r.normalScale       = m.normalScale;
        r.occlusionStrength = m.occlusionStrength;
    }

    Vec<TextureBinding> texRecs(alloc, Tag::Cook);
    texRecs.resize(d.textures.size);
    for (usize i = 0; i < d.textures.size; ++i) {
        TextureBindingDesc const& t = d.textures[i];
        TextureBinding& r           = texRecs[i];
        r.textureId = (t.flags & kTextureExternal) ? 0 : t.textureId ? t.textureId : hash_name(t.path);
        r.pathStr   = strs.intern(t.path);
        r.slot      = u8(t.slot);
        r.uvSet     = t.uvSet;
        r.flags     = t.flags;
    }

    Vec<Mount> mountRecs(alloc, Tag::Cook);
    mountRecs.resize(nMounts);
    for (u32 k = 0; k < nMounts; ++k) {
        MountDesc const& m = d.mounts[mountOrder[k]];
        Mount& r           = mountRecs[k];
        r.nameStr          = strs.intern(m.name);
        r.parentPart       = m.parentPart;
        r.nameHash         = mountHash[mountOrder[k]];
        std::memcpy(r.translation, m.translation, sizeof r.translation);
        std::memcpy(r.rotation, m.rotation, sizeof r.rotation);
    }
    for (u32 k = 0; k < nMounts; ++k) {
        MountDesc const& m     = d.mounts[mountOrder[k]];
        mountRecs[k].extrasStr = m.extras.empty() ? kInvalid : strs.intern(m.extras);
    }
    strs.finish();
    if (strs.bytes().size() >= kLimit32) KILN_WRITE_FAIL(kDiagHeaderSizes, "string table exceeds 4 GiB");

    Vec<VertexLayout> layRecs(alloc, Tag::Cook);
    layRecs.append(d.layouts);
    for (VertexLayout& l : layRecs) {
        l._reserved = 0;
        for (VertexAttrib& a : l.attribs) {
            a._pad  = 0;
            a._pad2 = 0;
        }
    }
    Vec<Submesh> submRecs(alloc, Tag::Cook);
    submRecs.append(d.submeshes);
    for (Submesh& s : submRecs)
        clear_bounds_pad(s.bounds);

    Vec<PayloadBlob> blobRecs(alloc, Tag::Cook);
    blobRecs.resize(blobs.size());
    for (usize i = 0; i < blobs.size(); ++i)
        blobRecs[i] = blobs[i].rec;

    // -- file layout ---------------------------------------------------------------
    SectionOut secs[12];
    u32 nSecs = 0;
    auto add  = [&](u32 id, void const* data, usize count, u32 stride) {
        secs[nSecs++] = SectionOut{id, data, u64(count) * stride, u32(count), stride, 0};
    };
    add(kSecModel, &model, 1, u32(sizeof(ModelInfo)));
    secs[nSecs++] = SectionOut{kSecStrings, strs.bytes().data(), strs.bytes().size(), 0, 0, 0};
    add(kSecLayouts, layRecs.data(), layRecs.size(), u32(sizeof(VertexLayout)));
    add(kSecParts, partRecs.data(), partRecs.size(), u32(sizeof(MeshPart)));
    add(kSecLods, lodRecs.data(), lodRecs.size(), u32(sizeof(MeshLod)));
    add(kSecSubmeshes, submRecs.data(), submRecs.size(), u32(sizeof(Submesh)));
    add(kSecMaterials, matRecs.data(), matRecs.size(), u32(sizeof(MaterialSlot)));
    if (!texRecs.empty()) add(kSecTextures, texRecs.data(), texRecs.size(), u32(sizeof(TextureBinding)));
    if (!mountRecs.empty()) add(kSecMounts, mountRecs.data(), mountRecs.size(), u32(sizeof(Mount)));
    add(kSecBlobs, blobRecs.data(), blobRecs.size(), u32(sizeof(PayloadBlob)));

    u32 const sectionCount = nSecs + 1; // + GPUD
    u64 fileCursor         = sizeof(FileHeader) + u64(sectionCount) * sizeof(SectionEntry);
    for (u32 i = 0; i < nSecs; ++i) {
        fileCursor     = align16(fileCursor);
        secs[i].offset = fileCursor;
        fileCursor += secs[i].bytes;
    }
    u64 const gpuDataOffset = align_up(fileCursor, u64(kPayloadBaseAlign));
    u64 const fileSize      = gpuDataOffset + gpuDataSize;

    FileHeader h{};
    h.magic              = kMagic;
    h.versionMajor       = kVersionMajor;
    h.versionMinor       = kVersionMinor;
    h.flags              = raw ? u32(kPayloadRaw) : 0u;
    h.sectionCount       = sectionCount;
    h.fileSize           = fileSize;
    h.sectionTableOffset = sizeof(FileHeader);
    h.sourceHash         = d.sourceHash;
    h.cookHash           = d.cookHash;
    h.gpuDataOffset      = gpuDataOffset;
    h.gpuDataSize        = gpuDataSize;
    h.payloadDecodedSize = decodedSize;
    h.payloadAlignment   = opt.payloadAlignment;
    h._reserved          = 0;

    Vec<u8> out(alloc, Tag::Cook);
    out.resize(usize(fileSize)); // zero-filled: every gap and padding byte stays 0
    u8* base = out.data();
    write_unaligned(base, h);
    for (u32 i = 0; i < nSecs; ++i) {
        SectionEntry e{};
        e.id     = secs[i].id;
        e.offset = secs[i].offset;
        e.size   = secs[i].bytes;
        e.count  = secs[i].count;
        e.stride = secs[i].stride;
        write_unaligned(base + sizeof(FileHeader) + usize(i) * sizeof(SectionEntry), e);
        if (secs[i].bytes) std::memcpy(base + secs[i].offset, secs[i].data, usize(secs[i].bytes));
    }
    SectionEntry gpud{};
    gpud.id     = kSecGpuData;
    gpud.offset = gpuDataOffset;
    gpud.size   = gpuDataSize;
    write_unaligned(base + sizeof(FileHeader) + usize(nSecs) * sizeof(SectionEntry), gpud);
    for (BlobOut const& b : blobs)
        std::memcpy(base + gpuDataOffset + b.rec.encodedOffset, b.encoded.empty() ? b.src : b.encoded.data(),
                    b.rec.encodedSize);

    if (stats) {
        stats->blobCount   = u32(blobs.size());
        stats->fileSize    = fileSize;
        stats->decodedSize = decodedSize;
        stats->encodedSize = gpuDataSize;
        stats->raw         = raw;
    }
    return Result<Vec<u8>>(std::move(out));
}

#undef KILN_WRITE_FAIL

} // namespace kiln::mesh
