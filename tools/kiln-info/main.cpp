// tools/kiln-info/main.cpp — dump a cooked .mesh or .ktx2 file, or a store manifest. Options: README.md.
// Exit codes: 0 ok, 1 usage, 2 file could not be read, 3 open/validation failed, 4 --check failed.
#include "cli.h"

#include "kiln/containers.h"
#include "kiln/ktx2.h"
#include "kiln/log.h"
#include "kiln/manifest.h"
#include "kiln/mesh.h"

#include <cstdio>
#include <cstring>

using namespace kiln;

namespace {

struct Options {
    char const* path = nullptr;
    bool blobs       = false;
    bool check       = false;
    bool quiet       = false;
};

bool g_quiet = false;

void out(char const* fmt, ...) KILN_PRINTF(1, 2);
void out(char const* fmt, ...) {
    if (g_quiet) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
}

void diag_to_stderr(void*, Diagnostic const& d) {
    std::fprintf(stderr, "%s: K%04u %.*s%s%.*s%s%.*s (%s)\n", severity_name(d.severity), unsigned(d.code),
                 KILN_SV(d.asset), d.asset.empty() ? "" : " ", KILN_SV(d.where), d.where.empty() ? "" : ": ",
                 KILN_SV(d.message), code_name(d.status.code));
}

bool read_file(char const* path, Vec<u8>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out.resize(usize(n));
    usize got = n ? std::fread(out.data(), 1, usize(n), f) : 0;
    std::fclose(f);
    return got == usize(n);
}

char const* fmt_name(u32 f) { return format_name(Format(f)); }

// ---------------------------------------------------------------------------
// .mesh
// ---------------------------------------------------------------------------

void print_bounds(char const* label, mesh::Bounds const& b) {
    out("%s center (%g %g %g) radius %g half (%g %g %g)\n", label, f64(b.center[0]), f64(b.center[1]),
        f64(b.center[2]), f64(b.radius), f64(b.halfExtents[0]), f64(b.halfExtents[1]), f64(b.halfExtents[2]));
}

void print_part_tree(mesh::MeshView const& v, u32 parent, int depth) {
    for (u32 i = 0; i < v.parts().size(); ++i) {
        mesh::MeshPart const& p = v.parts()[i];
        if (p.parent != parent) continue;
        out("  %*s[%u] %.*s  hash %016llx  lods [%u, +%u)  t(%g %g %g) q(%g %g %g %g)\n", depth * 2, "", i,
            KILN_SV(v.str(p.nameStr)), static_cast<unsigned long long>(p.nameHash), p.lodFirst, p.lodCount,
            f64(p.translation[0]), f64(p.translation[1]), f64(p.translation[2]), f64(p.rotation[0]),
            f64(p.rotation[1]), f64(p.rotation[2]), f64(p.rotation[3]));
        if (p.posScale[0] != 1.f || p.posScale[1] != 1.f || p.posScale[2] != 1.f || p.posBias[0] != 0.f ||
            p.posBias[1] != 0.f || p.posBias[2] != 0.f)
            out("  %*s    dequant scale (%g %g %g) bias (%g %g %g)\n", depth * 2, "", f64(p.posScale[0]),
                f64(p.posScale[1]), f64(p.posScale[2]), f64(p.posBias[0]), f64(p.posBias[1]),
                f64(p.posBias[2]));
        print_part_tree(v, i, depth + 1);
    }
}

int dump_mesh(Span<u8 const> bytes, Options const& o, DiagSink const* diag) {
    Result<mesh::MeshView> r = mesh::MeshView::open(bytes, {.validate = true}, diag, StrView(o.path));
    if (r.failed()) {
        std::fprintf(stderr, "%s: not a valid .mesh (%s)\n", o.path, code_name(r.code()));
        return 3;
    }
    mesh::MeshView const& v   = *r;
    mesh::FileHeader const& h = v.header();

    out("kiln .mesh  %s\n", o.path);
    out("  version %u.%u  flags 0x%x%s  file %llu B  sections %u\n", h.versionMajor, h.versionMinor, h.flags,
        v.payload_raw() ? " (raw payload)" : "", static_cast<unsigned long long>(h.fileSize), h.sectionCount);
    out("  source xxh64 %016llx  cook %016llx\n", static_cast<unsigned long long>(h.sourceHash),
        static_cast<unsigned long long>(h.cookHash));
    out("  GPUD @ %llu  encoded %llu B  decoded %llu B  align %u\n",
        static_cast<unsigned long long>(h.gpuDataOffset), static_cast<unsigned long long>(h.gpuDataSize),
        static_cast<unsigned long long>(h.payloadDecodedSize), h.payloadAlignment);

    out("\nsections\n");
    for (mesh::SectionEntry const& s : v.sections()) {
        char id[5];
        fourcc_str(s.id, id);
        out("  %s  @ %8llu  %8llu B  count %6u  stride %u\n", id, static_cast<unsigned long long>(s.offset),
            static_cast<unsigned long long>(s.size), s.count, s.stride);
    }

    out("\nmodel  \"%.*s\"  assetId %016llx  flags 0x%x\n", KILN_SV(v.name()),
        static_cast<unsigned long long>(v.asset_id()), v.model().flags);
    print_bounds("  bounds", v.model().bounds);

    out("\nlayouts (%u)\n", v.layouts().size());
    for (u32 i = 0; i < v.layouts().size(); ++i) {
        mesh::VertexLayout const& l = v.layouts()[i];
        out("  [%u] streams %u:", i, l.streamCount);
        for (u32 s = 0; s < l.streamCount; ++s)
            out(" %uB", l.strides[s]);
        out("\n");
        for (u32 a = 0; a < l.attribCount; ++a) {
            mesh::VertexAttrib const& at = l.attribs[a];
            out("      %s%u  stream %u @%u  %s\n", mesh::semantic_name(mesh::Semantic(at.semantic)),
                at.semanticIndex, at.stream, at.offset, fmt_name(at.format));
        }
    }

    out("\nparts (%u)\n", v.parts().size());
    print_part_tree(v, kInvalid, 0);

    out("\nlods (%u)\n", v.lods().size());
    for (u32 i = 0; i < v.lods().size(); ++i) {
        mesh::MeshLod const& l = v.lods()[i];
        out("  [%u] layout %u  verts %u  indices %u %s @%u  submeshes [%u, +%u)  err %g\n", i, l.layout,
            l.vertexCount, l.indexCount, mesh::index_type_name(mesh::IndexType(l.indexType)), l.indexOffset,
            l.submeshFirst, l.submeshCount, f64(l.geometricError));
        out("      streams @");
        for (u32 s = 0; s < mesh::kMaxStreams; ++s) {
            if (l.streamOffset[s] == kInvalid)
                out(" -");
            else
                out(" %u(%llu B)", l.streamOffset[s], static_cast<unsigned long long>(v.stream_bytes(l, s)));
        }
        out("\n");
        for (u32 s = 0; s < l.submeshCount; ++s) {
            mesh::Submesh const& sm = v.submeshes()[l.submeshFirst + s];
            out("      sub[%u] material %u  indices [%u, +%u)  vertexBase %d\n", l.submeshFirst + s,
                sm.material, sm.indexFirst, sm.indexCount, sm.vertexBase);
        }
    }

    out("\nmaterials (%u)\n", v.materials().size());
    for (u32 i = 0; i < v.materials().size(); ++i) {
        mesh::MaterialSlot const& m = v.materials()[i];
        out("  [%u] \"%.*s\"  hash %016llx  %s cutoff %g  flags 0x%x\n", i, KILN_SV(v.str(m.nameStr)),
            static_cast<unsigned long long>(m.nameHash), mesh::alpha_mode_name(mesh::AlphaMode(m.alphaMode)),
            f64(m.alphaCutoff), m.flags);
        out("      baseColor %g %g %g %g  metallic %g  roughness %g  emissive %g %g %g  normalScale %g  "
            "occlusion %g\n",
            f64(m.baseColorFactor[0]), f64(m.baseColorFactor[1]), f64(m.baseColorFactor[2]),
            f64(m.baseColorFactor[3]), f64(m.metallicFactor), f64(m.roughnessFactor),
            f64(m.emissiveFactor[0]), f64(m.emissiveFactor[1]), f64(m.emissiveFactor[2]), f64(m.normalScale),
            f64(m.occlusionStrength));
        for (u32 t = 0; t < m.textureCount; ++t) {
            mesh::TextureBinding const& tb = v.textures()[m.textureFirst + t];
            if (tb.flags & mesh::kTextureExternal)
                out("      %-10s uv%u %s uri \"%.*s\"\n", mesh::texture_slot_name(mesh::TextureSlot(tb.slot)),
                    tb.uvSet, (tb.flags & mesh::kTextureSrgb) ? "sRGB  " : "linear",
                    KILN_SV(v.str(tb.pathStr)));
            else
                out("      %-10s uv%u %s \"%.*s\"  id %016llx\n",
                    mesh::texture_slot_name(mesh::TextureSlot(tb.slot)), tb.uvSet,
                    (tb.flags & mesh::kTextureSrgb) ? "sRGB  " : "linear", KILN_SV(v.str(tb.pathStr)),
                    static_cast<unsigned long long>(tb.textureId));
        }
    }

    if (!v.mounts().empty()) {
        out("\nmounts (%u)\n", v.mounts().size());
        for (u32 i = 0; i < v.mounts().size(); ++i) {
            mesh::Mount const& m = v.mounts()[i];
            out("  [%u] \"%.*s\"  hash %016llx  parent %d  t(%g %g %g) q(%g %g %g %g)", i,
                KILN_SV(v.str(m.nameStr)), static_cast<unsigned long long>(m.nameHash),
                m.parentPart == kInvalid ? -1 : int(m.parentPart), f64(m.translation[0]),
                f64(m.translation[1]), f64(m.translation[2]), f64(m.rotation[0]), f64(m.rotation[1]),
                f64(m.rotation[2]), f64(m.rotation[3]));
            if (m.extrasStr != kInvalid) out("  extras \"%.*s\"", KILN_SV(v.str(m.extrasStr)));
            out("\n");
        }
    }

    out("\nblobs (%u)%s\n", v.blobs().size(), o.blobs ? "" : "  (use --blobs for the table)");
    if (o.blobs) {
        out("  %4s %10s %10s %10s %10s %5s %-8s %-12s %5s %4s %8s\n", "#", "encOff", "encSize", "decOff",
            "decSize", "elem", "codec", "filter", "flags", "rank", "xxh32");
        for (u32 i = 0; i < v.blobs().size(); ++i) {
            mesh::PayloadBlob const& b = v.blobs()[i];
            out("  %4u %10u %10u %10u %10u %5u %-8s %-12s %5x %4u %08x\n", i, b.encodedOffset, b.encodedSize,
                b.decodedOffset, b.decodedSize, b.elementSize, mesh::codec_name(mesh::Codec(b.codec)),
                mesh::filter_name(mesh::Filter(b.filter)), b.flags, b.lodRank, b.checksum);
        }
    }

    if (o.check) {
        if (v.encoded().empty()) {
            std::fprintf(stderr, "%s: --check needs the whole file (GPUD missing)\n", o.path);
            return 4;
        }
        Vec<u8> dst(default_allocator(), Tag::Payload);
        dst.resize(usize(v.decoded_size()));
        Status st = mesh::decode_payload(v, v.encoded(), dst.span(), {.verifyChecksums = true}, diag, nullptr,
                                         StrView(o.path));
        if (st.failed()) {
            std::fprintf(stderr, "%s: payload decode failed (%s)\n", o.path, code_name(st.code));
            return 4;
        }
        st = mesh::check_indices(v, dst.span(), diag, StrView(o.path));
        if (st.failed()) {
            std::fprintf(stderr, "%s: index check failed (%s)\n", o.path, code_name(st.code));
            return 4;
        }
        out("\ncheck: payload decoded (%llu B), checksums and index values OK\n",
            static_cast<unsigned long long>(v.decoded_size()));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// .ktx2
// ---------------------------------------------------------------------------

void print_kv(void*, StrView key, Span<u8 const> value) {
    // Print printable values as text, others as byte counts.
    bool text = value.size > 0 && value[value.size - 1] == 0;
    for (usize i = 0; text && i + 1 < value.size; ++i)
        if (value[i] < 0x20 || value[i] > 0x7e) text = false;
    if (text)
        out("  %.*s = \"%s\"\n", KILN_SV(key), reinterpret_cast<char const*>(value.data));
    else
        out("  %.*s = <%llu bytes>\n", KILN_SV(key), static_cast<unsigned long long>(value.size));
}

int dump_ktx2(Span<u8 const> bytes, Options const& o, DiagSink const* diag) {
    Result<ktx2::Ktx2View> r = ktx2::Ktx2View::open(bytes, diag, StrView(o.path));
    if (r.failed()) {
        std::fprintf(stderr, "%s: not a valid KTX2 (%s)\n", o.path, code_name(r.code()));
        return 3;
    }
    ktx2::Ktx2View const& v = *r;
    ktx2::Header const& h   = v.header();
    ktx2::TextureDesc d     = v.desc();

    out("KTX2  %s\n", o.path);
    out("  format %s (vkFormat %u)  typeSize %u  %ux%ux%u  layers %u%s  faces %u%s  levels %u%s\n",
        format_name(d.format), h.vkFormat, h.typeSize, d.width, d.height, d.depth, d.layers,
        d.isArray ? " (array)" : "", d.faces, d.isCube ? " (cube)" : "", d.levels,
        v.wants_generated_mips() ? " (file asks for generated mips)" : "");
    out("  supercompression %s  dfd @%u %u B (transfer %s, %u samples)  kvd @%u %u B  sgd @%llu %llu B\n",
        ktx2::supercompression_name(ktx2::Supercompression(h.supercompressionScheme)), h.dfdByteOffset,
        h.dfdByteLength,
        v.dfd_transfer_function() == 2   ? "sRGB"
        : v.dfd_transfer_function() == 1 ? "linear"
                                         : "?",
        v.dfd_sample_count(), h.kvdByteOffset, h.kvdByteLength,
        static_cast<unsigned long long>(h.sgdByteOffset), static_cast<unsigned long long>(h.sgdByteLength));

    out("\nlevels\n");
    Span<ktx2::LevelIndex const> lv = v.levels();
    for (u32 i = 0; i < lv.size; ++i) {
        out("  [%u] %ux%u  @ %llu  %llu B", i, v.level_width(i), v.level_height(i),
            static_cast<unsigned long long>(lv[i].byteOffset),
            static_cast<unsigned long long>(lv[i].byteLength));
        if (v.supercompressed())
            out(" of %llu (%.2fx)", static_cast<unsigned long long>(lv[i].uncompressedByteLength),
                f64(lv[i].uncompressedByteLength) / f64(lv[i].byteLength));
        out("%s\n", v.level_data(i).empty() ? "  (data not in buffer)" : "");
    }

    if (v.has_kvd()) {
        out("\nkey/value\n");
        v.for_each_key(&print_kv, nullptr);
    }

    if (o.check) {
        if (!v.has_all_level_data()) {
            std::fprintf(stderr, "%s: --check: level data missing from the file\n", o.path);
            return 4;
        }
        Vec<u8> texels(default_allocator(), Tag::General);
        for (u32 i = 0; i < lv.size; ++i) {
            u64 expect = v.level_image_bytes(i) * d.faces * d.layers;
            if (lv[i].uncompressedByteLength != expect) {
                std::fprintf(stderr, "%s: level %u is %llu bytes, expected %llu\n", o.path, i,
                             static_cast<unsigned long long>(lv[i].uncompressedByteLength),
                             static_cast<unsigned long long>(expect));
                return 4;
            }
            texels.resize(usize(expect));
            if (v.decode_level(i, texels.span(), nullptr, diag, StrView(o.path)).failed()) return 4;
        }
        out("\ncheck: %u levels present with expected sizes%s\n", unsigned(lv.size),
            v.supercompressed() ? ", every Zstd frame decodes" : "");
    }
    return 0;
}

// manifest

int dump_manifest(Span<u8 const> bytes, Options const& o, DiagSink const* diag) {
    Result<ManifestView> r = ManifestView::open(bytes, diag, StrView(o.path));
    if (r.failed()) {
        std::fprintf(stderr, "%s: not a valid manifest (%s)\n", o.path, code_name(r.code()));
        return 3;
    }
    ManifestView const& v = *r;
    out("kiln manifest  %s\n", o.path);
    out("  %u profile(s)\n", v.profile_count());
    for (u32 p = 0; p < v.profile_count(); ++p) {
        ManifestProfile const pr = v.profile(p);
        out("\nprofile %.*s, hash %016llx, block formats %016llx, %llu entries\n", KILN_SV(pr.name()),
            static_cast<unsigned long long>(pr.hash()), static_cast<unsigned long long>(pr.block_formats()),
            static_cast<unsigned long long>(pr.size()));
        for (u64 i = 0; i < pr.size(); ++i) {
            ManifestEntry const e = pr.entry(i);
            char key[33];
            hash128_hex(e.key, key);
            out("  %-7s %s %10llu B  %.*s\n", e.kind == AssetKind::Mesh ? "mesh" : "texture", key,
                static_cast<unsigned long long>(e.bytes), KILN_SV(e.name));
        }
    }
    if (!o.check) return 0;

    // The store is the manifest's directory: <store>/manifest.dir.
    StrView store(o.path);
    usize const slash = store.rfind('/') != StrView::kNpos ? store.rfind('/') : store.rfind('\\');
    store             = slash == StrView::kNpos ? StrView(".") : store.substr(0, slash);
    Vec<u8> artifact(default_allocator(), Tag::Io);
    for (u32 p = 0; p < v.profile_count(); ++p) {
        ManifestProfile const pr = v.profile(p);
        for (u64 i = 0; i < pr.size(); ++i) {
            ManifestEntry const e = pr.entry(i);
            char path[1200];
            (void)artifact_file_path(store, e.key, path, sizeof path);
            artifact.clear();
            if (!read_file(path, artifact) || artifact.size() != e.bytes ||
                !(xxh3_128(artifact.span()) == e.checksum)) {
                std::fprintf(stderr, "%s: %.*s: %.*s: the artifact %s is missing or has other bytes\n",
                             o.path, KILN_SV(pr.name()), KILN_SV(e.name), path);
                return 4;
            }
        }
    }
    out("\ncheck: every artifact of every profile is present with its size and checksum\n");
    return 0;
}

bool set_path(void* user, char const* arg) {
    auto* o = static_cast<Options*>(user);
    if (o->path) {
        std::fprintf(stderr, "kiln-info: one file at a time ('%s' after '%s')\n", arg, o->path);
        return false;
    }
    o->path = arg;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    cli::Option const opts[] = {
        {.name = "--blobs", .help = "print the full BLOB table (default: summary only)",    .flag = &o.blobs},
        {.name = "--check",
         .help = ".mesh: decode the payload and verify checksums and indices; .ktx2: verify every level; "
                 "manifest: verify every artifact",                                         .flag = &o.check},
        {.name = "--quiet", .help = "errors only (the exit code still reports the result)", .flag = &o.quiet},
    };
    cli::Spec const spec{
        .program  = "kiln-info",
        .synopsis = "<file.mesh|file.ktx2|manifest.dir> [options]",
        .options  = {opts, countof(opts)},
        .footer = "Exit codes: 0 ok, 1 usage, 2 file could not be read, 3 open/validation failed, 4 --check "
                  "failed.",
        .positional = &set_path,
        .user       = &o,
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok || !o.path) {
        cli::usage(spec, stderr);
        return 1;
    }
    g_quiet = o.quiet;

    DiagSink diag{&diag_to_stderr, nullptr};

    Vec<u8> bytes(default_allocator(), Tag::Io);
    if (!read_file(o.path, bytes)) {
        std::fprintf(stderr, "%s: cannot read file\n", o.path);
        return 2;
    }
    Span<u8 const> span = bytes.span();

    if (span.size >= 4 && read_unaligned<u32>(span.data) == mesh::kMagic) return dump_mesh(span, o, &diag);
    if (span.size >= 12 && std::memcmp(span.data, ktx2::kIdentifier, 12) == 0)
        return dump_ktx2(span, o, &diag);
    if (span.size >= 4 && read_unaligned<u32>(span.data) == kManifestMagic)
        return dump_manifest(span, o, &diag);

    std::fprintf(stderr, "%s: unknown file type (not KMSH, KTX2 or KMAN)\n", o.path);
    return 3;
}
