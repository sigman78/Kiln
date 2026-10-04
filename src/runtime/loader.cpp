// loader.cpp — worker side: one job runs the meta or the upload stage of one asset.
// See docs/design/threading-and-io.md.
#include "runtime_internal.h"

#include "../formats/formats_internal.h"

#include <cstdarg>

namespace kiln::rt {
namespace {

// Worker diagnostics go into the slot's DiagCapture; pump() emits them.
void capture_fn(void* user, Diagnostic const& d) {
    auto* c = static_cast<DiagCapture*>(user);
    // Keep the first error (or the first diagnostic if no error arrives).
    if (c->set && (c->severity == Severity::Error || d.severity != Severity::Error)) return;
    c->set      = true;
    c->code     = d.code;
    c->severity = d.severity;
    if (d.where.empty())
        format(c->msg, sizeof c->msg, "K%04u %.*s", d.code, KILN_SV(d.message));
    else
        format(c->msg, sizeof c->msg, "K%04u %.*s: %.*s", d.code, KILN_SV(d.where), KILN_SV(d.message));
}

void note(DiagCapture& c, char const* fmt, ...) KILN_PRINTF(2, 3);
void note(DiagCapture& c, char const* fmt, ...) {
    if (c.set) return;
    va_list args;
    va_start(args, fmt);
    vformat(c.msg, sizeof c.msg, fmt, args);
    va_end(args);
    c.set      = true;
    c.code     = 0;
    c.severity = Severity::Error;
}

/// Holds `n` bytes of the in-flight read budget (ContextDesc::ioInFlightBytes). Waits
/// while the budget is exhausted. A read larger than the whole budget proceeds when
/// nothing else is in flight, so this never deadlocks (holders never wait on the pump thread).
class IoBytes {
public:
    IoBytes(Context* ctx, u64 n) : ctx_(ctx), n_(n) {
        std::atomic<u64>& used = ctx->ioBytesInFlight;
        u64 cur                = used.load(std::memory_order_relaxed);
        for (;;) {
            if (cur == 0 || cur + n <= ctx->ioBudget) {
                if (used.compare_exchange_weak(cur, cur + n, std::memory_order_acq_rel)) return;
                continue; // cur reloaded
            }
            used.wait(cur, std::memory_order_relaxed); // until a holder releases
            cur = used.load(std::memory_order_relaxed);
        }
    }
    // Holders are jobs, which the context outlives, so notifying after the release is safe.
    ~IoBytes() {
        ctx_->ioBytesInFlight.fetch_sub(n_, std::memory_order_acq_rel);
        ctx_->ioBytesInFlight.notify_all();
    }
    IoBytes(IoBytes const&)            = delete;
    IoBytes& operator=(IoBytes const&) = delete;

private:
    Context* ctx_;
    u64 n_;
};

/// A store file or a memory span (registered bytes / cook output). Closes its file when it goes out
/// of scope.
struct Source {
    IoBackend const* io = nullptr;
    IoFile file;
    Span<u8 const> mem;
    bool memory = false;
    u64 size    = 0;

    Source()                         = default;
    Source(Source const&)            = delete;
    Source& operator=(Source const&) = delete;
    ~Source() { close(); }

    Status read(u64 off, u64 n, void* dst) const {
        if (off > size || n > size - off) return make_status(Code::IoEof);
        if (n == 0) return kOk;
        if (memory) {
            std::memcpy(dst, mem.data + off, usize(n));
            return kOk;
        }
        return io->read_range(io->user, file, off, n, dst);
    }
    void close() {
        if (!memory && file.valid()) io->close(io->user, file);
        file = {};
    }
};

/// What one read opens: the slot's own asset, or one layer of a texture array. The pointers are the
/// job fields that hold its artifact key and any cook output.
struct Input {
    AssetKind kind = AssetKind::Texture;
    StrView name;
    Hash128* key             = nullptr;
    bool* keyValid           = nullptr;
    Vec<u8>* cooked          = nullptr;
    bool* cookedValid        = nullptr;
    bool* providerOwned      = nullptr;
    Buffer const* registered = nullptr; ///< register_*() bytes; null for store entries
};

Input slot_input(Slot& s) {
    return {s.kind,
            path_of(s),
            &s.out.key,
            &s.out.keyValid,
            &s.out.cooked,
            &s.out.cookedValid,
            &s.out.providerOwned,
            s.source == SourceKind::Memory ? &s.memory : nullptr};
}

Input layer_input(Slot& s, ArrayLayer& l) {
    return {AssetKind::Texture, s.array->name(l), &l.jobKey,           &l.jobKeyValid,
            &l.cooked,          &l.cookedValid,   &l.jobProviderOwned, nullptr};
}

/// The provider checks the asset and names its artifact, or cooks it into `*in.cooked`, which then
/// serves both stages.
Status prepare_source(Context* ctx, Slot& s, Input const& in, Source& src) {
    Vec<u8> out(ctx->alloc, Tag::Payload);
    Hash128 key;
    DiagSink sink{&capture_fn, &s.out.capture};
    // May take seconds when it cooks; we are on a worker.
    ProfileZone const zone(ctx->prof, "kiln.prepare", in.name);
    PrepareMode const mode = s.in.recheck ? PrepareMode::Recheck : PrepareMode::Normal;
    Status const st =
        s.in.provider.prepare(s.in.provider.user, in.kind, in.name, mode, ctx->alloc, &out, &key, &sink);
    if (st.failed()) {
        if (st.code == Code::NotFound) {
            note(s.out.capture, "the cook provider found no source");
            s.out.diag = kDiagStoreMiss;
        } else {
            note(s.out.capture, "cook provider failed");
            s.out.diag        = kDiagCookOnMissFailed;
            *in.providerOwned = true; // a fix reaches it through the provider
        }
        return s.out.status = st;
    }
    // Neither bytes nor a key: the manifest entry chosen at dispatch stands (or the miss).
    if (!key.is_zero()) {
        *in.key      = key;
        *in.keyValid = true;
    }
    if (out.empty()) return kOk;
    if (key.is_zero()) { // bytes without an artifact (memory mode)
        *in.keyValid      = false;
        *in.providerOwned = true;
    }
    *in.cooked      = std::move(out);
    *in.cookedValid = true;
    src.memory      = true;
    src.mem         = in.cooked->span();
    src.size        = src.mem.size;
    return kOk;
}

/// Resolve an input: the artifact the manifest named at dispatch, or the one the cook provider
/// names (the meta stage asks it first when one is installed).
Status open_source(Context* ctx, Slot& s, Input const& in, Source& src, bool allowCook) {
    if (*in.cookedValid || in.registered) {
        src.memory = true;
        src.mem    = *in.cookedValid ? in.cooked->span() : in.registered->span();
        src.size   = src.mem.size;
        return kOk;
    }
    if (allowCook && s.in.provider.prepare) {
        KILN_TRY(prepare_source(ctx, s, in, src));
        if (src.memory) return kOk;
    }
    if (!*in.keyValid) {
        // Roots name sources, but only a cook provider reads them.
        char const* const hint =
            ctx->rootCount && !s.in.provider.prepare ? "; no cook provider is installed" : "";
        if (s.in.manifestPresent)
            note(s.out.capture, "not in profile '%s' of the store's manifest%s", ctx->profile, hint);
        else
            note(s.out.capture, "the store has no manifest, or no profile '%s' in it%s", ctx->profile, hint);
        s.out.diag          = s.in.manifestPresent ? kDiagStoreMiss : kDiagManifestMissing;
        return s.out.status = make_status(Code::NotFound);
    }

    char file[1024];
    usize const n = artifact_file_path(StrView(ctx->storeDir, ctx->storeDirLen), *in.key, file, sizeof file);
    if (n + 1 >= sizeof file) {
        note(s.out.capture, "store path too long");
        s.out.diag          = kDiagAssetLoadFailed;
        return s.out.status = make_status(Code::InvalidArgument);
    }
    IoBackend const* io = ctx->io;
    Status st           = io->open(io->user, StrView(file, n), &src.file);
    if (st.ok()) {
        src.io = io;
        st     = io->size(io->user, src.file, &src.size);
        if (st.failed()) {
            src.close();
            note(s.out.capture, "cannot stat '%s'", file);
            s.out.diag          = kDiagAssetLoadFailed;
            return s.out.status = st;
        }
        return kOk;
    }
    if (st.code == Code::NotFound)
        note(s.out.capture, "the manifest names '%s', which is missing", file);
    else
        note(s.out.capture, "cannot open '%s'", file);
    s.out.diag          = st.code == Code::NotFound ? kDiagStoreMiss : kDiagAssetLoadFailed;
    return s.out.status = st;
}

// ---------------------------------------------------------------------------
// Meta stage
// ---------------------------------------------------------------------------

Status mesh_meta(Context* ctx, Slot& s, Source const& src) {
    DiagSink const sink{&capture_fn, &s.out.capture};
    StrView const name = path_of(s);
    alignas(16) u8 hb[sizeof(mesh::FileHeader)];
    u64 const hn = min<u64>(sizeof hb, src.size);
    KILN_TRY(src.read(0, hn, hb));
    if (hn < sizeof hb) { // let the reader explain (truncated)
        Result<mesh::MeshView> r = mesh::MeshView::open(Span<u8 const>(hb, usize(hn)), {}, &sink, name);
        return r.failed() ? r.status() : make_status(Code::Corrupt);
    }
    mesh::FileHeader const h = read_unaligned<mesh::FileHeader>(hb);
    if (h.magic != mesh::kMagic || h.gpuDataOffset < sizeof hb || h.gpuDataOffset > src.size) {
        Result<mesh::MeshView> r = mesh::MeshView::open(Span<u8 const>(hb, sizeof hb), {}, &sink, name);
        if (r.failed()) return r.status();
        return diagf(&sink, make_status(Code::Corrupt), mesh::kDiagHeaderSizes, Severity::Error, name,
                     "header", "gpuDataOffset %llu outside the file (%llu bytes)",
                     static_cast<unsigned long long>(h.gpuDataOffset),
                     static_cast<unsigned long long>(src.size));
    }
    if (h.gpuDataSize > src.size - h.gpuDataOffset)
        return diagf(&sink, make_status(Code::Corrupt), mesh::kDiagTruncated, Severity::Error, name, "header",
                     "file is %llu bytes, header needs %llu", static_cast<unsigned long long>(src.size),
                     static_cast<unsigned long long>(h.gpuDataOffset + h.gpuDataSize));

    MetaSet& m = s.out.next;
    m.meta.allocate(ctx->alloc, usize(h.gpuDataOffset), Tag::Payload);
    {
        IoBytes budget(ctx, h.gpuDataOffset);
        KILN_TRY(src.read(0, h.gpuDataOffset, m.meta.data));
    }
    mesh::OpenOptions opt;
    opt.validate             = true;
    Result<mesh::MeshView> r = mesh::MeshView::open(m.meta.span(), opt, &sink, name);
    if (r.failed()) return r.status();
    m.meshView   = *r;
    m.uploadSize = h.payloadDecodedSize;
    return kOk;
}

/// A KTX2 input's description and, per level, [srcOffset | srcLength | texelLength]. texelLength is
/// larger than srcLength when the levels are Zstd frames.
struct KtxLevels {
    ktx2::TextureDesc desc;
    u64* cols  = nullptr; ///< 3 * levels values, from ctx->alloc
    u32 levels = 0;
    bool zstd  = false;
    void release(Allocator const* a) {
        if (cols) free_array(a, cols, usize(levels) * 3, Tag::Payload);
        cols = nullptr;
    }
};

/// Reads and checks a KTX2 input's metadata. `layer` is kInvalid for a texture of its own, else the
/// array layer being read (which must be 2D).
Status read_ktx2_levels(Context* ctx, Slot& s, StrView name, Source const& src, TextureShape expect,
                        u32 layer, KtxLevels* out) {
    DiagSink const sink{&capture_fn, &s.out.capture};
    alignas(16) u8 hb[sizeof(ktx2::Header)];
    u64 const hn = min<u64>(sizeof hb, src.size);
    KILN_TRY(src.read(0, hn, hb));
    ktx2::Header h{};
    std::memcpy(&h, hb, usize(hn));
    u64 const msize = hn == sizeof hb ? ktx2::Ktx2View::metadata_size(h) : 0;
    if (hn < sizeof hb || msize < sizeof hb || msize > src.size || msize > (u64(64) << 20)) {
        Result<ktx2::Ktx2View> r = ktx2::Ktx2View::open(Span<u8 const>(hb, usize(hn)), &sink, name);
        if (r.failed()) return r.status();
        return diagf(&sink, make_status(Code::Corrupt), ktx2::kDiagKtxTruncated, Severity::Error, name,
                     "header", "metadata prefix of %llu bytes does not fit the file (%llu bytes)",
                     static_cast<unsigned long long>(msize), static_cast<unsigned long long>(src.size));
    }

    Buffer prefix;
    prefix.allocate(ctx->alloc, usize(msize), Tag::Io);
    Status st = src.read(0, msize, prefix.data);
    Result<ktx2::Ktx2View> r =
        st.ok() ? ktx2::Ktx2View::open(prefix.span(), &sink, name) : Result<ktx2::Ktx2View>(st);
    if (r.failed()) {
        prefix.release();
        return r.status();
    }
    ktx2::Ktx2View const& v   = *r;
    ktx2::TextureDesc const d = v.desc();
    // Before the level checks: a volume or cube array is reported as a shape, not a layout.
    if (TextureShape const shape = shape_of(d); shape != expect) {
        prefix.release();
        if (layer == kInvalid) {
            note(s.out.capture, "the cooked texture is %s, the request expects %s", texture_shape_name(shape),
                 texture_shape_name(expect));
            s.out.diag = kDiagTextureShapeMismatch;
        } else {
            note(s.out.capture, "layer %u (%.*s) is %s; array layers must be 2D", layer, KILN_SV(name),
                 texture_shape_name(shape));
            s.out.diag = kDiagArrayLayerMismatch;
        }
        return make_status(Code::ValidationFailed);
    }
    u32 const levels     = d.levels;
    u64* cols            = alloc_array<u64>(ctx->alloc, usize(levels) * 3, Tag::Payload);
    out->desc            = d;
    out->cols            = cols;
    out->levels          = levels;
    out->zstd            = v.supercompressed();
    FormatInfo const& fi = v.info();
    for (u32 i = 0; i < levels && st.ok(); ++i) {
        ktx2::LevelIndex const& li = v.levels()[i];
        cols[i]                    = li.byteOffset;
        cols[levels + i]           = li.byteLength;
        cols[2 * levels + i]       = li.uncompressedByteLength;
        u64 const rowBytes         = format_row_bytes(d.format, v.level_width(i));
        u64 const rows = (u64(v.level_height(i)) + fi.blockHeight - 1) / fi.blockHeight * v.level_depth(i) *
                         d.layers * d.faces;
        if (li.uncompressedByteLength != rowBytes * rows)
            st = diagf(&sink, make_status(Code::Corrupt), ktx2::kDiagKtxLevelIndex, Severity::Error, name,
                       "levelIndex", "level %u holds %llu bytes, expected %llu", i,
                       static_cast<unsigned long long>(li.uncompressedByteLength),
                       static_cast<unsigned long long>(rowBytes * rows));
        else if (li.byteOffset > src.size || li.byteLength > src.size - li.byteOffset)
            st = diagf(&sink, make_status(Code::Corrupt), ktx2::kDiagKtxLevelIndex, Severity::Error, name,
                       "levelIndex", "level %u data lies outside the file (%llu bytes)", i,
                       static_cast<unsigned long long>(src.size));
    }
    prefix.release();
    if (st.failed()) out->release(ctx->alloc);
    return st;
}

Status texture_meta(Context* ctx, Slot& s, Source const& src) {
    KtxLevels k;
    KILN_TRY(read_ktx2_levels(ctx, s, path_of(s), src, s.texShape, kInvalid, &k));
    u32 const levels = k.levels;
    u64* layout      = alloc_array<u64>(ctx->alloc, usize(levels) * kLayoutColumns, Tag::Payload);
    std::memcpy(layout + 2 * levels, k.cols, sizeof(u64) * levels * 3);
    MetaSet& m     = s.out.next;
    m.layout       = layout;
    m.layoutLevels = levels;
    m.texZstd      = k.zstd;
    m.texDesc      = k.desc;
    m.uploadSize   = texture_layout(k.desc, ctx->cc.optimalRowPitchAlign, ctx->cc.optimalOffsetAlign, layout,
                                    layout + levels);
    k.release(ctx->alloc);
    return kOk;
}

/// Puts the failing layer in front of the captured reason.
void note_layer(DiagCapture& c, u32 layer, StrView name) {
    char reason[sizeof c.msg];
    format(reason, sizeof reason, "%s", c.set ? c.msg : "load failed");
    format(c.msg, sizeof c.msg, "layer %u (%.*s): %s", layer, KILN_SV(name), reason);
    if (!c.set) {
        c.set      = true;
        c.code     = 0;
        c.severity = Severity::Error;
    }
}

/// Every layer must match layer 0 in format, size and level count.
Status check_layer(Slot& s, u32 layer, StrView name, ktx2::TextureDesc const& first,
                   ktx2::TextureDesc const& d) {
    if (d.format == first.format && d.width == first.width && d.height == first.height &&
        d.levels == first.levels)
        return kOk;
    note(s.out.capture, "layer %u (%.*s) is %s %ux%u with %u levels; layer 0 is %s %ux%u with %u levels",
         layer, KILN_SV(name), format_name(d.format), d.width, d.height, d.levels, format_name(first.format),
         first.width, first.height, first.levels);
    s.out.diag = kDiagArrayLayerMismatch;
    return make_status(Code::ValidationFailed);
}

/// Each layer's metadata, checked against layer 0, then the upload layout of the whole array. Each
/// layer keeps its level table for the upload stage.
CompletionKind run_array_meta(Context* ctx, Slot& s) {
    ArrayDecl& d = *s.array;
    KtxLevels first;
    Status st = kOk;
    for (u32 i = 0; i < d.count && st.ok(); ++i) {
        ArrayLayer& l  = d.layers[i];
        Input const in = layer_input(s, l);
        Source src;
        KtxLevels k;
        st = open_source(ctx, s, in, src, true);
        if (st.ok()) st = read_ktx2_levels(ctx, s, in.name, src, TextureShape::Tex2D, i, &k);
        src.close();
        if (st.ok() && i > 0) st = check_layer(s, i, in.name, first.desc, k.desc);
        if (st.failed()) {
            k.release(ctx->alloc);
            if (s.out.diag != kDiagArrayLayerMismatch) note_layer(s.out.capture, i, in.name);
            break;
        }
        l.srcLevels = k.levels;
        l.src       = alloc_array<u64>(ctx->alloc, usize(k.levels) * 2, Tag::Payload);
        std::memcpy(l.src, k.cols, sizeof(u64) * k.levels * 2);
        l.zstd = k.zstd;
        if (i == 0)
            first = k; // its texel lengths go into the layout
        else
            k.release(ctx->alloc);
    }
    if (st.ok()) {
        u32 const levels  = first.levels;
        MetaSet& m        = s.out.next;
        m.texDesc         = first.desc;
        m.texDesc.layers  = d.count;
        m.texDesc.faces   = 1;
        m.texDesc.isArray = true;
        m.layout          = alloc_array<u64>(ctx->alloc, usize(levels) * kLayoutColumns, Tag::Payload);
        m.layoutLevels    = levels;
        std::memset(m.layout + 2 * levels, 0, sizeof(u64) * levels * 2); // per layer: ArrayLayer::src
        std::memcpy(m.layout + 4 * levels, first.cols + 2 * levels, sizeof(u64) * levels);
        m.uploadSize = texture_layout(m.texDesc, ctx->cc.optimalRowPitchAlign, ctx->cc.optimalOffsetAlign,
                                      m.layout, m.layout + levels);
        if (m.uploadSize == 0) {
            note(s.out.capture, "%u layers of %s %ux%u do not fit one upload", d.count,
                 format_name(first.desc.format), first.desc.width, first.desc.height);
            s.out.diag = kDiagArrayDeclaration;
            st         = make_status(Code::InvalidArgument);
        }
    }
    first.release(ctx->alloc);
    if (st.failed()) {
        s.out.status = st;
        if (s.out.diag == 0) s.out.diag = kDiagAssetLoadFailed;
        return CompletionKind::Failed;
    }
    return CompletionKind::MetaReady;
}

CompletionKind run_meta(Context* ctx, Slot& s) {
    if (s.array) return run_array_meta(ctx, s);
    Source src;
    if (open_source(ctx, s, slot_input(s), src, true).failed()) return CompletionKind::Failed;
    Status const st = s.kind == AssetKind::Mesh ? mesh_meta(ctx, s, src) : texture_meta(ctx, s, src);
    src.close();
    if (st.failed()) {
        s.out.status = st;
        if (s.out.diag == 0) s.out.diag = kDiagAssetLoadFailed;
        return CompletionKind::Failed;
    }
    return CompletionKind::MetaReady;
}

// ---------------------------------------------------------------------------
// Upload stage
// ---------------------------------------------------------------------------

Status write_mesh(Context* ctx, Slot& s, Source const& src, u8* dst) {
    DiagSink const sink{&capture_fn, &s.out.capture};
    StrView const name        = path_of(s);
    mesh::MeshView const& v   = s.out.next.meshView;
    mesh::FileHeader const& h = v.header();
    Span<u8> const out(dst, usize(h.payloadDecodedSize));
    if (h.gpuDataOffset > src.size || h.gpuDataSize > src.size - h.gpuDataOffset)
        return make_status(Code::IoEof);
    if (v.payload_raw() && !src.memory) {
        // Identity layout: one read of GPUD straight into the adapter's memory.
        if (h.payloadDecodedSize > h.gpuDataSize)
            return diagf(&sink, make_status(Code::Corrupt), mesh::kDiagPayloadRaw, Severity::Error, name,
                         "header", "raw payload larger than GPUD");
        IoBytes budget(ctx, h.payloadDecodedSize);
        return src.read(h.gpuDataOffset, h.payloadDecodedSize, dst);
    }
    Vec<u8> encoded(ctx->alloc, Tag::Io);
    Span<u8 const> gpud;
    if (src.memory) {
        gpud = src.mem.subspan(usize(h.gpuDataOffset), usize(h.gpuDataSize));
    } else {
        encoded.resize(usize(h.gpuDataSize));
        IoBytes budget(ctx, h.gpuDataSize);
        KILN_TRY(src.read(h.gpuDataOffset, h.gpuDataSize, encoded.data()));
        gpud = encoded.span();
    }
    mesh::DecodeOptions const dopt{.alloc = ctx->alloc};
    if (v.payload_raw()) return mesh::decode_payload(v, gpud, out, dopt, &sink, nullptr, name);
    // Never straight into the adapter's memory: Zstd reads its output back (as in write_level).
    Vec<u8> decoded(ctx->alloc, Tag::Io);
    decoded.resize(out.size);
    KILN_TRY(mesh::decode_payload(v, gpud, decoded.span(), dopt, &sink, nullptr, name));
    if (out.size) std::memcpy(out.data, decoded.data(), out.size);
    return kOk;
}

/// Buffers one texture write reuses across its levels (and layers).
struct LevelScratch {
    explicit LevelScratch(Allocator const* a) : scratch(a, Tag::Io), texels(a, Tag::Io), zstd(a) {}
    Vec<u8> scratch;
    Vec<u8> texels;
    fmt::ZstdDecoder zstd;
};

/// One stored level of one input: [sOff, sOff + sLen) in `src`, `tLen` texel bytes once decoded, written
/// to `out` with rows of `rowBytes` padded to `pitch`.
struct LevelCopy {
    u32 level     = 0;
    u64 sOff      = 0;
    u64 sLen      = 0;
    u64 tLen      = 0;
    u64 rowBytes  = 0;
    u64 pitch     = 0;
    bool zstd     = false;
    StrView input = {}; ///< for the diagnostic of a frame that does not decode
};

Status write_level(Context* ctx, Slot& s, Source const& src, LevelCopy const& c, u8* out, LevelScratch& sc) {
    u64 const rows    = c.rowBytes ? c.tLen / c.rowBytes : 0;
    bool const direct = c.pitch == c.rowBytes;
    if (direct && !c.zstd) {
        IoBytes budget(ctx, c.sLen);
        return src.read(c.sOff, c.sLen, out);
    }
    u8 const* from = nullptr;
    if (src.memory) {
        if (c.sOff > src.size || c.sLen > src.size - c.sOff) return make_status(Code::IoEof);
        from = src.mem.data + c.sOff;
    } else {
        sc.scratch.resize(usize(c.sLen));
        IoBytes budget(ctx, c.sLen);
        KILN_TRY(src.read(c.sOff, c.sLen, sc.scratch.data()));
        from = sc.scratch.data();
    }
    if (c.zstd) {
        // Never straight into the adapter's memory: staging is often write-combined, and Zstd reads
        // its output back for matches (cook-tracing.md, "First findings").
        sc.texels.resize(usize(c.tLen));
        if (!sc.zstd.decode(Span<u8 const>(from, usize(c.sLen)), sc.texels.span())) {
            DiagSink const sink{&capture_fn, &s.out.capture};
            return diagf(&sink, make_status(Code::Corrupt), ktx2::kDiagKtxLevelDecode, Severity::Error,
                         c.input, "levelIndex", "level %u does not decode: %s", c.level, sc.zstd.error());
        }
        from = sc.texels.data();
    }
    if (direct)
        std::memcpy(out, from, usize(c.tLen));
    else
        for (u64 r = 0; r < rows; ++r) {
            u8* row = out + r * c.pitch;
            std::memcpy(row, from + r * c.rowBytes, usize(c.rowBytes));
            std::memset(row + c.rowBytes, 0, usize(c.pitch - c.rowBytes));
        }
    return kOk;
}

/// Zero the gaps between levels and after the last one; `layers` layers of each level lie back to back.
void zero_level_gaps(MetaSet const& m, u32 layers, u8* dst) {
    u32 const levels = m.layoutLevels;
    u64 cursor       = 0;
    for (u32 i = 0; i < levels; ++i) {
        u64 const dOff     = m.layout[i];
        u64 const rowBytes = format_row_bytes(m.texDesc.format, max(m.texDesc.width >> i, 1u));
        u64 const rows     = rowBytes ? m.layout[4 * levels + i] / rowBytes : 0;
        if (dOff > cursor) std::memset(dst + cursor, 0, usize(dOff - cursor));
        cursor = dOff + m.layout[levels + i] * rows * layers;
    }
    if (cursor < m.uploadSize) std::memset(dst + cursor, 0, usize(m.uploadSize - cursor));
}

LevelCopy level_copy(MetaSet const& m, u32 i) {
    u32 const levels = m.layoutLevels;
    return {.level    = i,
            .tLen     = m.layout[4 * levels + i],
            .rowBytes = format_row_bytes(m.texDesc.format, max(m.texDesc.width >> i, 1u)),
            .pitch    = m.layout[levels + i]};
}

Status write_texture(Context* ctx, Slot& s, Source const& src, u8* dst) {
    MetaSet const& m = s.out.next;
    u32 const levels = m.layoutLevels;
    LevelScratch sc(ctx->alloc);
    zero_level_gaps(m, 1, dst);
    for (u32 i = 0; i < levels; ++i) {
        LevelCopy c = level_copy(m, i);
        c.sOff      = m.layout[2 * levels + i];
        c.sLen      = m.layout[3 * levels + i];
        c.zstd      = m.texZstd;
        c.input     = path_of(s);
        KILN_TRY(write_level(ctx, s, src, c, dst + m.layout[i], sc));
    }
    return kOk;
}

/// Each layer's levels into its place in the array: layer j of level i at the level's offset plus j
/// times one layer's padded size.
Status write_array(Context* ctx, Slot& s, u8* dst) {
    MetaSet const& m = s.out.next;
    ArrayDecl& d     = *s.array;
    u32 const levels = m.layoutLevels;
    LevelScratch sc(ctx->alloc);
    zero_level_gaps(m, d.count, dst);
    for (u32 j = 0; j < d.count; ++j) {
        ArrayLayer& l  = d.layers[j];
        Input const in = layer_input(s, l);
        Source src;
        Status st = open_source(ctx, s, in, src, false);
        for (u32 i = 0; i < levels && st.ok(); ++i) {
            LevelCopy c      = level_copy(m, i);
            c.sOff           = l.src[i];
            c.sLen           = l.src[l.srcLevels + i];
            c.zstd           = l.zstd;
            c.input          = in.name;
            u64 const rows   = c.rowBytes ? c.tLen / c.rowBytes : 0;
            u64 const stride = c.pitch * rows;
            st               = write_level(ctx, s, src, c, dst + m.layout[i] + j * stride, sc);
        }
        src.close();
        if (st.failed()) {
            note_layer(s.out.capture, j, in.name);
            return st;
        }
    }
    return kOk;
}

CompletionKind run_upload(Context* ctx, Slot& s) {
    Adapter const& a = ctx->adapter;
    MetaSet const& m = s.out.next;
    UploadDesc ud;
    ud.id   = s.id;
    ud.size = m.uploadSize;
    MeshPayloadDesc md;
    TextureDesc td;
    if (s.kind == AssetKind::Mesh) {
        mesh::FileHeader const& h = m.meshView.header();
        md.payloadDecodedSize     = h.payloadDecodedSize;
        md.payloadAlignment       = h.payloadAlignment;
        u32 indexSize             = 0;
        for (u32 i = 0; i < m.meshView.lods().size(); ++i)
            indexSize = max(indexSize, mesh::index_size(mesh::IndexType(m.meshView.lods()[i].indexType)));
        md.indexSize = indexSize ? indexSize : 4;
        ud.kind      = UploadKind::MeshPayload;
        ud.alignment = u32(max<u64>(h.payloadAlignment, ctx->cc.bufferOffsetAlign));
        ud.mesh      = &md;
    } else {
        ktx2::TextureDesc const& d = m.texDesc;
        td.format                  = d.format;
        td.width                   = d.width;
        td.height                  = d.height;
        td.depth                   = d.depth;
        td.layers                  = d.layers * d.faces; // cube faces are layers at the boundary
        td.shape                   = s.texShape;
        td.levels                  = d.levels;
        td.firstLevel              = 0;
        ud.kind                    = UploadKind::TextureLevels;
        ud.alignment               = u32(max<u64>(ctx->cc.optimalOffsetAlign, 16));
        ud.texture                 = &td;
    }

    UploadTarget t;
    Status st = a.begin_upload(a.user, ud, &t);
    if (st.code == Code::Busy) return CompletionKind::BusyRetry;
    if (st.failed()) {
        note(s.out.capture, "begin_upload failed (%llu bytes)", static_cast<unsigned long long>(ud.size));
        s.out.status = st;
        s.out.diag   = kDiagAdapterRejected;
        return CompletionKind::Failed;
    }
    s.out.target    = t;
    s.out.hasTarget = true;

    u32 diag = kDiagAssetLoadFailed;
    if (!t.dst && ud.size) {
        note(s.out.capture, "begin_upload returned no destination memory");
        st   = make_status(Code::Internal);
        diag = kDiagAdapterRejected;
    } else if (s.kind == AssetKind::Texture && t.rowPitchAlign > 1) {
        for (u32 i = 0; i < m.layoutLevels; ++i) {
            if (m.layout[m.layoutLevels + i] % t.rowPitchAlign != 0) {
                note(s.out.capture, "adapter row pitch alignment %llu differs from copy_constraints (%llu)",
                     static_cast<unsigned long long>(t.rowPitchAlign),
                     static_cast<unsigned long long>(ctx->cc.optimalRowPitchAlign));
                st   = make_status(Code::Unsupported);
                diag = kDiagAdapterRejected;
                break;
            }
        }
    }
    if (st.ok() && s.array) {
        st = write_array(ctx, s, static_cast<u8*>(t.dst));
        if (st.failed() && s.out.diag)
            diag = s.out.diag == kDiagStoreMiss ? kDiagAssetLoadFailed : s.out.diag;
    } else if (st.ok()) {
        Source src;
        st = open_source(ctx, s, slot_input(s), src, false);
        if (st.ok()) {
            u8* dst = static_cast<u8*>(t.dst);
            st = s.kind == AssetKind::Mesh ? write_mesh(ctx, s, src, dst) : write_texture(ctx, s, src, dst);
            src.close();
        } else {
            diag = s.out.diag == kDiagStoreMiss ? kDiagAssetLoadFailed : s.out.diag; // vanished since meta
        }
    }
    if (st.failed() && a.discard_upload) { // nothing for the GPU: the adapter frees ticket and object
        a.discard_upload(a.user, t.token);
        s.out.hasTarget = false;
    } else {
        a.commit_upload(a.user, t.token); // a failed load commits too; kiln destroys the result
    }
    if (st.failed()) {
        s.out.status = st;
        s.out.diag   = diag;
        return CompletionKind::Failed;
    }
    return CompletionKind::Uploaded;
}

void post(Context* ctx, Completion const& c) {
    std::lock_guard<std::mutex> lock(ctx->compMutex);
    KILN_VERIFY(ctx->compCount < ctx->compCap); // one job per slot: never full
    ctx->comp[(ctx->compHead + ctx->compCount) % ctx->compCap] = c;
    ++ctx->compCount;
}

} // namespace

usize manifest_path(Context const* ctx, char* out, usize cap) {
    return manifest_file_path(StrView(ctx->storeDir, ctx->storeDirLen), out, cap);
}

u64 texture_layout(ktx2::TextureDesc const& d, u64 pitchAlign, u64 offsetAlign, u64* outOffset,
                   u64* outPitch) {
    TextureDesc const t{.format     = d.format,
                        .width      = d.width,
                        .height     = d.height,
                        .depth      = d.depth,
                        .layers     = d.layers * d.faces,
                        .levels     = d.levels,
                        .shape      = TextureShape::Tex2D,
                        .firstLevel = 0};
    CopyConstraints const c{.optimalRowPitchAlign = pitchAlign, .optimalOffsetAlign = offsetAlign};
    return texture_level_layout(t, c, outOffset, outPitch);
}

void run_job(void* arg) {
    Slot& s      = *static_cast<Slot*>(arg);
    Context* ctx = s.ctx;
    s.out.status = kOk;
    s.out.diag   = 0;
    s.out.capture.reset();
    bool const meta = s.in.stage == Stage::Meta;
    CompletionKind k;
    {
        if (ctx->prof)
            profile_interval(ctx->prof, "kiln.wait.pool", path_of(s), s.submitNs, profile_now_ns());
        ProfileZone const zone(ctx->prof, meta ? "kiln.meta" : "kiln.upload", path_of(s));
        k = meta ? run_meta(ctx, s) : run_upload(ctx, s);
    }
    Completion const c{s.index, s.in.gen, k};
    post(ctx, c); // from here on the pump thread may reuse `s`
    ctx->jobsInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace kiln::rt

namespace kiln {

u64 unsampled_block_formats(Adapter const& a) {
    if (!a.supports_format) return 0;
    u64 excluded = 0;
    for (u32 v = u32(Format::BC1_RGB_UNORM); v <= u32(Format::ASTC_12x12_SRGB); ++v)
        if (format_info(Format(v)) && !a.supports_format(a.user, Format(v), FormatUsage::SampledImage))
            excluded |= block_format_bit(Format(v));
    return excluded;
}

u64 texture_level_layout(TextureDesc const& t, CopyConstraints const& c, u64* offsets, u64* pitches) {
    FormatInfo const* fi = format_info(t.format);
    if (!fi) return 0;
    constexpr u64 kMaxAlign = u64(1) << 32;
    u64 const pitchAlign    = std::bit_ceil(clamp<u64>(c.optimalRowPitchAlign, 1, kMaxAlign));
    u64 const offsetAlign   = std::bit_ceil(clamp<u64>(c.optimalOffsetAlign, 1, kMaxAlign));
    auto const extent       = [](u32 v, u32 level) { return level < 32 ? max(v >> level, 1u) : 1u; };
    u64 cur                 = 0;
    for (u32 i = 0; i < t.levels; ++i) {
        u32 const w = extent(t.width, i);
        u32 const h = extent(t.height, i);
        u32 const z = extent(t.depth, i);
        u64 pitch = 0, rows = 0, bytes = 0;
        if (!checked_add(format_row_bytes(t.format, w), pitchAlign - 1, pitch) ||
            !checked_mul((u64(h) + fi->blockHeight - 1) / fi->blockHeight, u64(z), rows) ||
            !checked_mul(rows, u64(max(t.layers, 1u)), rows) || !checked_add(cur, offsetAlign - 1, cur))
            return 0;
        pitch &= ~(pitchAlign - 1);
        cur &= ~(offsetAlign - 1);
        if (offsets) offsets[i] = cur;
        if (pitches) pitches[i] = pitch;
        if (!checked_mul(pitch, rows, bytes) || !checked_add(cur, bytes, cur)) return 0;
    }
    return cur;
}

} // namespace kiln
