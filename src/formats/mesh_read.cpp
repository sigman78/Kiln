// .mesh reader: MeshView::open, payload decode and tools checks (docs/mesh-format-spec.md v0.3).
#include "kiln/mesh.h"

#include "kiln/log.h"

namespace kiln::mesh {

char const* semantic_name(Semantic s) noexcept {
    switch (s) {
    case Semantic::Position: return "Position";
    case Semantic::Normal: return "Normal";
    case Semantic::Tangent: return "Tangent";
    case Semantic::TexCoord: return "TexCoord";
    case Semantic::Color: return "Color";
    case Semantic::Joints: return "Joints";
    case Semantic::Weights: return "Weights";
    case Semantic::Custom: return "Custom";
    }
    return "?";
}
char const* index_type_name(IndexType t) noexcept {
    switch (t) {
    case IndexType::U16: return "U16";
    case IndexType::U32: return "U32";
    case IndexType::U8: return "U8";
    }
    return "?";
}
char const* alpha_mode_name(AlphaMode m) noexcept {
    switch (m) {
    case AlphaMode::Opaque: return "Opaque";
    case AlphaMode::Mask: return "Mask";
    case AlphaMode::Blend: return "Blend";
    }
    return "?";
}
char const* texture_slot_name(TextureSlot s) noexcept {
    switch (s) {
    case TextureSlot::BaseColor: return "BaseColor";
    case TextureSlot::Normal: return "Normal";
    case TextureSlot::MetalRough: return "MetalRough";
    case TextureSlot::Occlusion: return "Occlusion";
    case TextureSlot::Emissive: return "Emissive";
    }
    return u8(s) < 16 ? "Reserved" : "?";
}
char const* codec_name(Codec c) noexcept {
    switch (c) {
    case Codec::None: return "None";
    case Codec::Zstd: return "Zstd";
    case Codec::MeshoptVertex: return "MeshoptVertex";
    case Codec::MeshoptIndex: return "MeshoptIndex";
    case Codec::MeshoptIndexSeq: return "MeshoptIndexSeq";
    }
    return "?";
}
char const* filter_name(Filter f) noexcept {
    switch (f) {
    case Filter::None: return "None";
    case Filter::ByteShuffle: return "ByteShuffle";
    case Filter::Delta: return "Delta";
    case Filter::MeshoptOct: return "MeshoptOct";
    case Filter::MeshoptQuat: return "MeshoptQuat";
    case Filter::MeshoptExp: return "MeshoptExp";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

namespace {

constexpr u32 kKnownCodecMax  = u32(Codec::MeshoptIndexSeq);
constexpr u32 kKnownFilterMax = u32(Filter::MeshoptExp);

struct Ctx {
    DiagSink const* diag;
    StrView asset;
};

#define KILN_MESH_FAIL(ctx, code, diagCode, ...)                                                             \
    return diagf((ctx).diag, make_status(Code::code), diagCode, Severity::Error, (ctx).asset, StrView{},     \
                 __VA_ARGS__)

/// Section lookup during open (before the view is built).
SectionEntry const* find_sec(Span<SectionEntry const> secs, u32 id) noexcept {
    for (SectionEntry const& s : secs)
        if (s.id == id) return &s;
    return nullptr;
}

/// Validate a record section's stride/count against T and bind a Records<T>.
template <class T>
Status bind_records(Ctx& ctx, Span<u8 const> bytes, SectionEntry const* sec, char const* name, bool required,
                    u32 minCount, Records<T>& out) noexcept {
    if (!sec) {
        if (required) KILN_MESH_FAIL(ctx, Corrupt, kDiagSectionMissing, "required section %s missing", name);
        out = {};
        return kOk;
    }
    if (sec->count < minCount)
        KILN_MESH_FAIL(ctx, Corrupt, kDiagSectionMissing, "section %s has %u records, need >= %u", name,
                       sec->count, minCount);
    if (sec->count == 0) {
        out = {};
        return kOk;
    }
    if (sec->stride < sizeof(T) || (sec->stride % 4) != 0)
        KILN_MESH_FAIL(ctx, VersionMismatch, kDiagRecordStride, "section %s stride %u (record is %u bytes)",
                       name, sec->stride, unsigned(sizeof(T)));
    if (u64(sec->count) * sec->stride > sec->size)
        KILN_MESH_FAIL(ctx, Corrupt, kDiagRecordStride, "section %s: %u x %u bytes exceeds section size %llu",
                       name, sec->count, sec->stride, static_cast<unsigned long long>(sec->size));
    out = Records<T>(bytes.data + sec->offset, sec->count, sec->stride);
    return kOk;
}

/// STRS ends with a NUL, so any in-range offset is a valid C string.
bool str_ok(Span<u8 const> strs, u32 off) noexcept { return off == kInvalid || off < strs.size; }

/// Sum of the intersection of [a, a+n) with every blob's decoded range.
u64 covered_bytes(Records<PayloadBlob> const& blobs, u64 a, u64 n) noexcept {
    u64 b = a + n, sum = 0;
    for (PayloadBlob const& pb : blobs) {
        u64 s = pb.decodedOffset, e = s + pb.decodedSize;
        u64 lo = s > a ? s : a, hi = e < b ? e : b;
        if (hi > lo) sum += hi - lo;
    }
    return sum;
}

Status validate_blobs(Ctx& ctx, FileHeader const& h, Records<PayloadBlob> const& blobs) noexcept {
    u64 prevEncEnd    = 0;
    u32 prevRank      = 0;
    bool decAscending = true;
    u64 prevDecEnd    = 0;
    for (u32 i = 0; i < blobs.size(); ++i) {
        PayloadBlob const& b = blobs[i];
        if ((b.encodedOffset % kBlobAlign) != 0 || (b.decodedOffset % kBlobAlign) != 0)
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagBlobEncoding, "blob %u: offsets not 16-byte aligned",
                           i);
        if (u64(b.encodedOffset) + b.encodedSize > h.gpuDataSize)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagBlobTable, "blob %u: encoded range outside GPUD", i);
        if (u64(b.decodedOffset) + b.decodedSize > h.payloadDecodedSize)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagBlobTable, "blob %u: decoded range outside payload", i);
        if (i > 0 && b.encodedOffset < prevEncEnd)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagBlobTable,
                           "blob %u: table not sorted by encodedOffset or ranges overlap", i);
        if (i > 0 && b.lodRank < prevRank)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagBlobTable, "blob %u: lodRank decreases along the table", i);
        if (b.codec > kKnownCodecMax || b.filter > kKnownFilterMax)
            KILN_MESH_FAIL(ctx, Unsupported, kDiagBlobUnsupported, "blob %u: unknown codec %u / filter %u", i,
                           unsigned(b.codec), unsigned(b.filter));
        Codec c  = Codec(b.codec);
        Filter f = Filter(b.filter);
        if (!is_allowed_blob_encoding(c, f))
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagBlobEncoding,
                           "blob %u: codec %s with filter %s not allowed", i, codec_name(c), filter_name(f));
        if ((b.flags & kBlobOuterZstd) && !is_meshopt_codec(c))
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagBlobEncoding, "blob %u: outer Zstd with codec %s", i,
                           codec_name(c));
        if (b.flags & ~u16(kBlobOuterZstd))
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagBlobEncoding, "blob %u: unknown flags 0x%x", i,
                           b.flags);
        if (b.elementSize == 0)
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagBlobEncoding, "blob %u: elementSize is 0", i);
        u32 unit = c == Codec::MeshoptIndex ? 3u * b.elementSize : b.elementSize;
        if ((b.decodedSize % unit) != 0)
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagBlobEncoding,
                           "blob %u: decodedSize %u not a multiple of %u", i, b.decodedSize, unit);
        if (b.elementSize == 1 && is_meshopt_codec(c))
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagBlobEncoding,
                           "blob %u: U8 indices with a meshopt codec", i);
        if (h.flags & kPayloadRaw) {
            if (c != Codec::None || f != Filter::None || b.flags != 0 || b.encodedOffset != b.decodedOffset ||
                b.encodedSize != b.decodedSize)
                KILN_MESH_FAIL(ctx, Corrupt, kDiagPayloadRaw,
                               "blob %u violates kPayloadRaw identity conditions", i);
        } else if (c == Codec::None && f == Filter::None && b.encodedSize != b.decodedSize) {
            KILN_MESH_FAIL(ctx, Corrupt, kDiagBlobTable, "blob %u: codec None but encodedSize != decodedSize",
                           i);
        }
        if (i > 0 && b.decodedOffset < prevDecEnd) decAscending = false;
        prevEncEnd = u64(b.encodedOffset) + b.encodedSize;
        prevDecEnd = u64(b.decodedOffset) + b.decodedSize;
        prevRank   = b.lodRank;
    }
    // Decoded ranges must not overlap. Writers lay them out ascending (checked above
    // in O(n)); otherwise fall back to a pairwise check.
    if (!decAscending) {
        for (u32 i = 0; i < blobs.size(); ++i) {
            u64 s0 = blobs[i].decodedOffset, e0 = s0 + blobs[i].decodedSize;
            for (u32 j = i + 1; j < blobs.size(); ++j) {
                u64 s1 = blobs[j].decodedOffset, e1 = s1 + blobs[j].decodedSize;
                if (s0 < e1 && s1 < e0)
                    KILN_MESH_FAIL(ctx, Corrupt, kDiagBlobTable, "blobs %u and %u: decoded ranges overlap", i,
                                   j);
            }
        }
    }
    return kOk;
}

Status validate_layouts(Ctx& ctx, Records<VertexLayout> const& layouts) noexcept {
    for (u32 li = 0; li < layouts.size(); ++li) {
        VertexLayout const& l = layouts[li];
        if (l.streamCount < 1 || l.streamCount > kMaxStreams || l.attribCount < 1 ||
            l.attribCount > kMaxAttribs)
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLayoutRule,
                           "layout %u: streamCount %u / attribCount %u", li, l.streamCount, l.attribCount);
        for (u32 s = 0; s < l.streamCount; ++s)
            if (l.strides[s] == 0 || (l.strides[s] % 4) != 0)
                KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLayoutRule, "layout %u: stream %u stride %u", li,
                               s, l.strides[s]);
        u32 stream0Attribs = 0;
        for (u32 a = 0; a < l.attribCount; ++a) {
            VertexAttrib const& at = l.attribs[a];
            if (at.stream >= l.streamCount)
                KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLayoutRule,
                               "layout %u attrib %u: stream %u out of range", li, a, at.stream);
            if (at.semantic > u8(Semantic::Custom))
                KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLayoutRule, "layout %u attrib %u: semantic %u", li,
                               a, at.semantic);
            FormatInfo const* fi = format_info(Format(at.format));
            if (!fi || fi->compressed)
                KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLayoutRule,
                               "layout %u attrib %u: format %u not a vertex format", li, a, at.format);
            if (u32(at.offset) + fi->bytesPerBlock > l.strides[at.stream])
                KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLayoutRule,
                               "layout %u attrib %u: offset %u + %u > stride %u", li, a, at.offset,
                               fi->bytesPerBlock, l.strides[at.stream]);
            if (at.stream == 0) {
                ++stream0Attribs;
                if (at.semantic != u8(Semantic::Position))
                    KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLayoutRule,
                                   "layout %u: stream 0 must contain only Position", li);
            }
        }
        if (stream0Attribs != 1)
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLayoutRule,
                           "layout %u: stream 0 must hold exactly one attribute", li);
    }
    return kOk;
}

Status validate_full(Ctx& ctx, MeshView const& v) noexcept {
    FileHeader const& h = v.header();
    Span<u8 const> strs = v.strings();
    u64 dec             = h.payloadDecodedSize;

    if (!str_ok(strs, v.model().nameStr) || v.model().nameStr == kInvalid)
        KILN_MESH_FAIL(ctx, Corrupt, kDiagStringOffset, "MODL nameStr out of range");

    KILN_TRY(validate_layouts(ctx, v.layouts()));

    auto const& parts = v.parts();
    for (u32 i = 0; i < parts.size(); ++i) {
        MeshPart const& p = parts[i];
        if (!str_ok(strs, p.nameStr)) KILN_MESH_FAIL(ctx, Corrupt, kDiagStringOffset, "part %u nameStr", i);
        if (p.parent != kInvalid && p.parent >= i)
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagPartOrder, "part %u: parent %u not before it", i,
                           p.parent);
        if (u64(p.lodFirst) + p.lodCount > v.lods().size() && p.lodCount != 0)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagIndexRange, "part %u: lods [%u, +%u) out of range", i,
                           p.lodFirst, p.lodCount);
        if (p.lodCount != 0 && p.lodFirst >= v.lods().size())
            KILN_MESH_FAIL(ctx, Corrupt, kDiagIndexRange, "part %u: lodFirst %u out of range", i, p.lodFirst);
    }

    auto const& lods = v.lods();
    for (u32 i = 0; i < lods.size(); ++i) {
        MeshLod const& l = lods[i];
        if (l.layout >= v.layouts().size())
            KILN_MESH_FAIL(ctx, Corrupt, kDiagIndexRange, "lod %u: layout %u out of range", i, l.layout);
        VertexLayout const& lay = v.layouts()[l.layout];
        for (u32 s = 0; s < kMaxStreams; ++s) {
            if (s < lay.streamCount) {
                if (l.streamOffset[s] == kInvalid)
                    KILN_MESH_FAIL(ctx, Corrupt, kDiagLodRange, "lod %u: stream %u has no offset", i, s);
                if ((l.streamOffset[s] % kBlobAlign) != 0)
                    KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLodRange,
                                   "lod %u: stream %u offset not 16-byte aligned", i, s);
                u64 bytes = u64(l.vertexCount) * lay.strides[s];
                if (u64(l.streamOffset[s]) + bytes > dec)
                    KILN_MESH_FAIL(ctx, Corrupt, kDiagLodRange, "lod %u: stream %u outside decoded payload",
                                   i, s);
                if (covered_bytes(v.blobs(), l.streamOffset[s], bytes) != bytes)
                    KILN_MESH_FAIL(ctx, Corrupt, kDiagLodRange,
                                   "lod %u: stream %u not fully covered by blobs", i, s);
            } else if (l.streamOffset[s] != kInvalid) {
                KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLodRange,
                               "lod %u: unused stream %u must be kInvalid", i, s);
            }
        }
        if (l.indexType > u8(IndexType::U8))
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagLodRange, "lod %u: indexType %u", i, l.indexType);
        u32 isz = index_size(IndexType(l.indexType));
        if ((l.indexOffset % isz) != 0)
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagIndexAlign,
                           "lod %u: indexOffset %u not a multiple of %u", i, l.indexOffset, isz);
        u64 ibytes = u64(l.indexCount) * isz;
        if (u64(l.indexOffset) + ibytes > dec)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagLodRange, "lod %u: index range outside decoded payload", i);
        if (ibytes && covered_bytes(v.blobs(), l.indexOffset, ibytes) != ibytes)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagLodRange, "lod %u: index range not fully covered by blobs", i);
        if (u64(l.submeshFirst) + l.submeshCount > v.submeshes().size())
            KILN_MESH_FAIL(ctx, Corrupt, kDiagIndexRange, "lod %u: submeshes [%u, +%u) out of range", i,
                           l.submeshFirst, l.submeshCount);
        for (u32 s = 0; s < l.submeshCount; ++s) {
            Submesh const& sm = v.submeshes()[l.submeshFirst + s];
            if (sm.material >= v.materials().size())
                KILN_MESH_FAIL(ctx, Corrupt, kDiagIndexRange, "lod %u submesh %u: material %u out of range",
                               i, s, sm.material);
            if (u64(sm.indexFirst) + sm.indexCount > l.indexCount)
                KILN_MESH_FAIL(ctx, Corrupt, kDiagIndexRange,
                               "lod %u submesh %u: indices [%u, +%u) exceed lod", i, s, sm.indexFirst,
                               sm.indexCount);
        }
    }

    auto const& mats = v.materials();
    for (u32 i = 0; i < mats.size(); ++i) {
        MaterialSlot const& m = mats[i];
        if (!str_ok(strs, m.nameStr))
            KILN_MESH_FAIL(ctx, Corrupt, kDiagStringOffset, "material %u nameStr", i);
        if (m.alphaMode > u8(AlphaMode::Blend))
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagIndexRange, "material %u: alphaMode %u", i,
                           m.alphaMode);
        if (u64(m.textureFirst) + m.textureCount > v.textures().size())
            KILN_MESH_FAIL(ctx, Corrupt, kDiagIndexRange, "material %u: textures [%u, +%u) out of range", i,
                           m.textureFirst, m.textureCount);
    }

    auto const& texs = v.textures();
    for (u32 i = 0; i < texs.size(); ++i) {
        TextureBinding const& t = texs[i];
        if (!str_ok(strs, t.pathStr))
            KILN_MESH_FAIL(ctx, Corrupt, kDiagStringOffset, "texture %u pathStr", i);
        if (t.slot > 15)
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagIndexRange, "texture %u: slot %u", i, t.slot);
        if ((t.flags & kTextureExternal) && t.textureId != 0)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagIndexRange, "texture %u: external binding with a textureId", i);
    }

    auto const& mounts = v.mounts();
    for (u32 i = 0; i < mounts.size(); ++i) {
        Mount const& m = mounts[i];
        if (!str_ok(strs, m.nameStr) || !str_ok(strs, m.extrasStr))
            KILN_MESH_FAIL(ctx, Corrupt, kDiagStringOffset, "mount %u string offset", i);
        if (m.parentPart != kInvalid && m.parentPart >= parts.size())
            KILN_MESH_FAIL(ctx, Corrupt, kDiagIndexRange, "mount %u: parentPart %u out of range", i,
                           m.parentPart);
        if (i > 0 && m.nameHash < mounts[i - 1].nameHash)
            KILN_MESH_FAIL(ctx, ValidationFailed, kDiagMountOrder, "mounts not sorted by nameHash at %u", i);
    }
    return kOk;
}

} // namespace

Result<MeshView> MeshView::open(Span<u8 const> bytes, OpenOptions const& opt, DiagSink const* diag,
                                StrView assetName) noexcept {
    Ctx ctx{diag, assetName};
    if (bytes.size < sizeof(FileHeader))
        KILN_MESH_FAIL(ctx, Corrupt, kDiagTruncated, "buffer of %llu bytes is smaller than the header",
                       static_cast<unsigned long long>(bytes.size));
    if ((reinterpret_cast<std::uintptr_t>(bytes.data) % 8) != 0)
        KILN_MESH_FAIL(ctx, InvalidArgument, kDiagBufferAlignment, "buffer must be 8-byte aligned");

    FileHeader const& h = *reinterpret_cast<FileHeader const*>(bytes.data);
    if (h.magic != kMagic) KILN_MESH_FAIL(ctx, Corrupt, kDiagBadMagic, "bad magic 0x%08x", h.magic);
    if (h.versionMajor != kVersionMajor || (kVersionMajor == 0 && h.versionMinor != kVersionMinor))
        KILN_MESH_FAIL(ctx, VersionMismatch, kDiagVersion, "file version %u.%u, loader %u.%u", h.versionMajor,
                       h.versionMinor, kVersionMajor, kVersionMinor);

    // Header arithmetic (all always-on checks, spec §7).
    if (h.fileSize < sizeof(FileHeader) || h.gpuDataOffset > h.fileSize ||
        h.gpuDataOffset < sizeof(FileHeader) || (h.gpuDataOffset % kPayloadBaseAlign) != 0 ||
        h.gpuDataSize != h.fileSize - h.gpuDataOffset)
        KILN_MESH_FAIL(ctx, Corrupt, kDiagHeaderSizes, "fileSize/gpuDataOffset/gpuDataSize inconsistent");
    if (h.payloadAlignment < kPayloadBaseAlign || !is_pow2(h.payloadAlignment))
        KILN_MESH_FAIL(ctx, Corrupt, kDiagHeaderSizes, "payloadAlignment %u", h.payloadAlignment);
    if (h.payloadDecodedSize > (u64(1) << 32) || h.gpuDataSize > (u64(1) << 32))
        KILN_MESH_FAIL(ctx, Corrupt, kDiagHeaderSizes, "payload exceeds the 4 GiB limit");
    if ((h.flags & kPayloadRaw) && h.gpuDataSize != h.payloadDecodedSize)
        KILN_MESH_FAIL(ctx, Corrupt, kDiagPayloadRaw, "kPayloadRaw but gpuDataSize != payloadDecodedSize");
    if (h.flags & ~u32(kPayloadRaw))
        KILN_MESH_FAIL(ctx, ValidationFailed, kDiagHeaderSizes, "unknown header flags 0x%x", h.flags);
    if (bytes.size < h.gpuDataOffset)
        KILN_MESH_FAIL(ctx, Corrupt, kDiagTruncated, "buffer holds %llu of the %llu-byte CPU region",
                       static_cast<unsigned long long>(bytes.size),
                       static_cast<unsigned long long>(h.gpuDataOffset));

    // Section table.
    u64 tableBytes = u64(h.sectionCount) * sizeof(SectionEntry);
    if (h.sectionCount == 0 || h.sectionTableOffset < sizeof(FileHeader) || (h.sectionTableOffset % 8) != 0 ||
        h.sectionTableOffset + tableBytes > h.gpuDataOffset)
        KILN_MESH_FAIL(ctx, Corrupt, kDiagSectionTable, "section table [%llu, +%llu) invalid",
                       static_cast<unsigned long long>(h.sectionTableOffset),
                       static_cast<unsigned long long>(tableBytes));

    MeshView v;
    v.header_   = &h;
    v.sections_ = {reinterpret_cast<SectionEntry const*>(bytes.data + h.sectionTableOffset), h.sectionCount};

    for (u32 i = 0; i < h.sectionCount; ++i) {
        SectionEntry const& s = v.sections_[i];
        for (u32 j = 0; j < i; ++j)
            if (v.sections_[j].id == s.id) {
                char id[5];
                fourcc_str(s.id, id);
                KILN_MESH_FAIL(ctx, Corrupt, kDiagSectionTable, "duplicate section %s", id);
            }
        if (s.id == kSecGpuData) {
            if (i != h.sectionCount - 1 || s.offset != h.gpuDataOffset || s.size != h.gpuDataSize)
                KILN_MESH_FAIL(ctx, Corrupt, kDiagSectionBounds, "GPUD must be last and match the header");
            continue;
        }
        if ((s.offset % kBlobAlign) != 0 || s.offset < sizeof(FileHeader) ||
            s.offset + s.size > h.gpuDataOffset || s.offset + s.size < s.offset) {
            char id[5];
            fourcc_str(s.id, id);
            KILN_MESH_FAIL(
                ctx, Corrupt, kDiagSectionBounds, "section %s [%llu, +%llu) outside the CPU region", id,
                static_cast<unsigned long long>(s.offset), static_cast<unsigned long long>(s.size));
        }
    }
    if (!find_sec(v.sections_, kSecGpuData))
        KILN_MESH_FAIL(ctx, Corrupt, kDiagSectionMissing, "GPUD missing");

    // Required blobs.
    SectionEntry const* strs = find_sec(v.sections_, kSecStrings);
    if (!strs) KILN_MESH_FAIL(ctx, Corrupt, kDiagSectionMissing, "STRS missing");
    if (strs->size == 0 || bytes.data[strs->offset + strs->size - 1] != 0)
        KILN_MESH_FAIL(ctx, Corrupt, kDiagStringOffset, "STRS must end with a null terminator");
    v.strings_ = bytes.subspan(strs->offset, strs->size);

    // Record sections.
    Records<ModelInfo> model;
    KILN_TRY(bind_records(ctx, bytes, find_sec(v.sections_, kSecModel), "MODL", true, 1, model));
    if (model.size() != 1)
        KILN_MESH_FAIL(ctx, Corrupt, kDiagSectionMissing, "MODL must hold exactly one record");
    v.model_ = &model[0];
    KILN_TRY(bind_records(ctx, bytes, find_sec(v.sections_, kSecLayouts), "LAYT", true, 1, v.layouts_));
    KILN_TRY(bind_records(ctx, bytes, find_sec(v.sections_, kSecParts), "PART", true, 1, v.parts_));
    KILN_TRY(bind_records(ctx, bytes, find_sec(v.sections_, kSecLods), "LODS", true, 0, v.lods_));
    KILN_TRY(bind_records(ctx, bytes, find_sec(v.sections_, kSecSubmeshes), "SUBM", true, 0, v.submeshes_));
    KILN_TRY(bind_records(ctx, bytes, find_sec(v.sections_, kSecMaterials), "MATL", true, 0, v.materials_));
    KILN_TRY(bind_records(ctx, bytes, find_sec(v.sections_, kSecTextures), "MTEX", false, 0, v.textures_));
    KILN_TRY(bind_records(ctx, bytes, find_sec(v.sections_, kSecMounts), "MNTS", false, 0, v.mounts_));
    KILN_TRY(bind_records(ctx, bytes, find_sec(v.sections_, kSecBlobs), "BLOB", true, 0, v.blobs_));

    if (bytes.size >= h.fileSize) v.encoded_ = bytes.subspan(h.gpuDataOffset, h.gpuDataSize);

    KILN_TRY(validate_blobs(ctx, h, v.blobs_));
    if (opt.validate) KILN_TRY(validate_full(ctx, v));
    return v;
}

SectionEntry const* MeshView::find_section(u32 id) const noexcept { return find_sec(sections_, id); }

StrView MeshView::str(u32 offset) const noexcept {
    if (offset == kInvalid || offset >= strings_.size) return {};
    char const* p = reinterpret_cast<char const*>(strings_.data + offset);
    return {p, StrView::cstr_len(p)}; // STRS ends with '\0' (checked in open)
}

Mount const* MeshView::find_mount(u64 nameHash) const noexcept {
    u32 lo = 0, hi = mounts_.size();
    while (lo < hi) {
        u32 mid        = lo + (hi - lo) / 2;
        Mount const& m = mounts_[mid];
        if (m.nameHash < nameHash)
            lo = mid + 1;
        else if (m.nameHash > nameHash)
            hi = mid;
        else
            return &m;
    }
    return nullptr;
}

MeshPart const* MeshView::find_part(u64 nameHash) const noexcept {
    for (MeshPart const& p : parts_)
        if (p.nameHash == nameHash) return &p;
    return nullptr;
}

u64 MeshView::stream_bytes(MeshLod const& lod, u32 s) const noexcept {
    if (s >= kMaxStreams || lod.layout >= layouts_.size()) return 0;
    VertexLayout const& lay = layouts_[lod.layout];
    if (s >= lay.streamCount) return 0;
    return u64(lod.vertexCount) * lay.strides[s];
}

Status decode_blob(PayloadBlob const& blob, Span<u8 const> encoded, Span<u8> dst, DecodeOptions const& opt,
                   DiagSink const* diag, Arena* /*scratch*/, StrView assetName) noexcept {
    Ctx ctx{diag, assetName};
    if (dst.size != blob.decodedSize)
        KILN_MESH_FAIL(ctx, InvalidArgument, kDiagDecodeSize, "destination is %llu bytes, blob decodes to %u",
                       static_cast<unsigned long long>(dst.size), blob.decodedSize);
    if (encoded.size != blob.encodedSize)
        KILN_MESH_FAIL(ctx, InvalidArgument, kDiagDecodeSize, "encoded span is %llu bytes, blob says %u",
                       static_cast<unsigned long long>(encoded.size), blob.encodedSize);

    Codec c  = Codec(blob.codec);
    Filter f = Filter(blob.filter);
    if (c == Codec::None && f == Filter::None && blob.flags == 0) {
        if (encoded.size != dst.size)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagDecodeSize, "codec None: encoded %u bytes != decoded %u",
                           blob.encodedSize, blob.decodedSize);
        if (dst.size) std::memcpy(dst.data, encoded.data, dst.size);
    } else {
        // v0.5 runtime: no compressed codecs or filters built in.
        KILN_MESH_FAIL(ctx, Unsupported, kDiagBlobUnsupported,
                       "codec %s / filter %s not supported by this runtime", codec_name(c), filter_name(f));
    }

    if (opt.verifyChecksums && blob.checksum != 0) {
        u32 h = xxh32(dst.data, dst.size);
        if (h != blob.checksum)
            KILN_MESH_FAIL(ctx, Corrupt, kDiagChecksum, "blob checksum 0x%08x, decoded 0x%08x", blob.checksum,
                           h);
    }
    return kOk;
}

Status decode_payload(MeshView const& v, Span<u8 const> gpud, Span<u8> dst, DecodeOptions const& opt,
                      DiagSink const* diag, Arena* scratch, StrView assetName) noexcept {
    Ctx ctx{diag, assetName};
    FileHeader const& h = v.header();
    if (gpud.size != h.gpuDataSize)
        KILN_MESH_FAIL(ctx, InvalidArgument, kDiagTruncated, "GPUD span is %llu bytes, header says %llu",
                       static_cast<unsigned long long>(gpud.size),
                       static_cast<unsigned long long>(h.gpuDataSize));
    if (dst.size < h.payloadDecodedSize)
        KILN_MESH_FAIL(
            ctx, InvalidArgument, kDiagDecodeSize, "destination %llu bytes < payloadDecodedSize %llu",
            static_cast<unsigned long long>(dst.size), static_cast<unsigned long long>(h.payloadDecodedSize));

    if (v.payload_raw()) {
        // Identity layout (validated by open): one copy. Gaps are zero in the file by contract.
        if (h.payloadDecodedSize) std::memcpy(dst.data, gpud.data, h.payloadDecodedSize);
        if (opt.verifyChecksums) {
            for (u32 i = 0; i < v.blobs().size(); ++i) {
                PayloadBlob const& b = v.blobs()[i];
                if (b.checksum == 0) continue;
                u32 x = xxh32(dst.data + b.decodedOffset, b.decodedSize);
                if (x != b.checksum)
                    KILN_MESH_FAIL(ctx, Corrupt, kDiagChecksum, "blob %u checksum 0x%08x, decoded 0x%08x", i,
                                   b.checksum, x);
            }
        }
        return kOk;
    }

    // General path: zero-fill everything (covers the gaps), then decode blob by blob.
    if (h.payloadDecodedSize) std::memset(dst.data, 0, h.payloadDecodedSize);
    for (u32 i = 0; i < v.blobs().size(); ++i) {
        PayloadBlob const& b = v.blobs()[i];
        Status st            = decode_blob(b, gpud.subspan(b.encodedOffset, b.encodedSize),
                                           dst.subspan(b.decodedOffset, b.decodedSize), opt, diag, scratch, assetName);
        if (st.failed()) return st;
    }
    return kOk;
}

Status check_indices(MeshView const& v, Span<u8 const> decoded, DiagSink const* diag,
                     StrView assetName) noexcept {
    Ctx ctx{diag, assetName};
    if (decoded.size < v.decoded_size())
        KILN_MESH_FAIL(ctx, InvalidArgument, kDiagDecodeSize, "decoded payload span too small");

    auto read_index = [&](MeshLod const& l, u32 k) -> u32 {
        u8 const* p = decoded.data + l.indexOffset;
        switch (IndexType(l.indexType)) {
        case IndexType::U16: return read_unaligned<u16>(p + usize(k) * 2);
        case IndexType::U32: return read_unaligned<u32>(p + usize(k) * 4);
        case IndexType::U8: return p[k];
        }
        return kInvalid;
    };

    for (u32 li = 0; li < v.lods().size(); ++li) {
        MeshLod const& l = v.lods()[li];
        // Independent of OpenOptions::validate: never read outside `decoded`.
        if (u64(l.indexOffset) + MeshView::index_bytes(l) > decoded.size ||
            u64(l.submeshFirst) + l.submeshCount > v.submeshes().size())
            KILN_MESH_FAIL(ctx, Corrupt, kDiagLodRange, "lod %u: index or submesh range out of bounds", li);
        for (u32 k = 0; k < l.indexCount; ++k) {
            u32 idx = read_index(l, k);
            if (idx >= l.vertexCount)
                KILN_MESH_FAIL(ctx, ValidationFailed, kDiagIndexValue,
                               "lod %u index %u = %u >= vertexCount %u", li, k, idx, l.vertexCount);
        }
        for (u32 s = 0; s < l.submeshCount; ++s) {
            Submesh const& sm = v.submeshes()[l.submeshFirst + s];
            for (u32 k = 0; k < sm.indexCount; ++k) {
                i64 eff = i64(read_index(l, sm.indexFirst + k)) + sm.vertexBase;
                if (eff < 0 || eff >= i64(l.vertexCount))
                    KILN_MESH_FAIL(ctx, ValidationFailed, kDiagIndexValue,
                                   "lod %u submesh %u: index + vertexBase = %lld outside [0, %u)", li, s,
                                   static_cast<long long>(eff), l.vertexCount);
            }
        }
    }
    return kOk;
}

#undef KILN_MESH_FAIL

} // namespace kiln::mesh
