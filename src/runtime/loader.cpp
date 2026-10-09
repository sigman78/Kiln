// loader.cpp — worker side: one job runs the meta or the upload stage of one asset.
// See docs/design/threading-and-io.md.
#include "runtime_internal.h"

#include "../formats/formats_internal.h"

#include <cstdarg>
#include <utility>

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
    bool memory              = false;
    u64 size                 = 0;
    ProfileHooks const* prof = nullptr; ///< a file's reads report "kiln.read" zones
    StrView name;

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
        ProfileZone const zone(prof, "kiln.read", name);
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

/// Registered bytes or cook output serve as the input: `src` reads them, and no file is opened.
[[nodiscard]] bool memory_source(Input const& in, Source& src) {
    if (!*in.cookedValid && !in.registered) return false;
    src.memory = true;
    src.mem    = *in.cookedValid ? in.cooked->span() : in.registered->span();
    src.size   = src.mem.size;
    return true;
}

/// The provider checks the asset and names its artifact, or cooks it into `*in.cooked`, which then
/// serves both stages. Without a provider, and for a memory source, it does nothing.
Status prepare_input(Context* ctx, Slot& s, Input const& in) {
    if (!s.in.provider.prepare || *in.cookedValid || in.registered) return kOk;
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
    return kOk;
}

/// Opens an input for reading: its memory, or the artifact that the manifest named at dispatch or
/// that the cook provider named since.
Status open_source(Context* ctx, Slot& s, Input const& in, Source& src) {
    if (memory_source(in, src)) return kOk;
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
    ProfileZone const zone(ctx->prof, "kiln.open", in.name);
    Status st = io->open(io->user, StrView(file, n), &src.file);
    if (st.ok()) {
        src.io   = io;
        src.prof = ctx->prof;
        src.name = in.name;
        st       = io->size(io->user, src.file, &src.size);
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

/// The lowest level whose width and height both fit `extent`; the last level when none does. 0 = the
/// first level (docs/design/streaming.md).
u32 first_level(ktx2::TextureDesc const& d, u32 extent) {
    if (extent == 0 || d.levels == 0) return 0;
    for (u32 i = 0; i < d.levels; ++i)
        if (max(d.width >> i, 1u) <= extent && max(d.height >> i, 1u) <= extent) return i;
    return d.levels - 1;
}

/// `d` at levels `first` to the last, as the GPU object holds them.
ktx2::TextureDesc resident_desc(ktx2::TextureDesc d, u32 first) {
    d.width  = max(d.width >> first, 1u);
    d.height = max(d.height >> first, 1u);
    d.depth  = max(d.depth >> first, 1u);
    d.levels = d.levels - first;
    return d;
}

Status texture_meta(Context* ctx, Slot& s, Source const& src) {
    KtxLevels k;
    KILN_TRY(read_ktx2_levels(ctx, s, path_of(s), src, s.texShape, kInvalid, &k));
    u32 const first  = first_level(k.desc, s.in.maxExtent);
    u32 const levels = k.levels - first;
    u64* layout      = alloc_array<u64>(ctx->alloc, usize(levels) * kLayoutColumns, Tag::Payload);
    for (u32 c = 0; c < 3; ++c)
        std::memcpy(layout + (2 + c) * levels, k.cols + c * k.levels + first, sizeof(u64) * levels);
    MetaSet& m      = s.out.next;
    m.layout        = layout;
    m.layoutLevels  = levels;
    m.texFirstLevel = first;
    m.texExtent     = s.in.maxExtent;
    m.texZstd       = k.zstd;
    m.texDesc       = k.desc;
    m.uploadSize    = texture_layout(resident_desc(k.desc, first), ctx->cc.optimalRowPitchAlign,
                                     ctx->cc.optimalOffsetAlign, layout, layout + levels);
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

/// The provider's work for every layer first. Then each layer's metadata, checked against layer 0,
/// and the upload layout of the whole array. Each layer keeps its level table for the upload stage.
CompletionKind run_array_meta(Context* ctx, Slot& s) {
    ArrayDecl& d = *s.array;
    KtxLevels first;
    u32 firstLevel = 0; // of layer 0; every layer has the same levels (check_layer)
    Status st      = kOk;
    for (u32 i = 0; i < d.count && st.ok(); ++i) {
        Input const in = layer_input(s, d.layers[i]);
        st             = prepare_input(ctx, s, in);
        if (st.failed()) note_layer(s.out.capture, i, in.name);
    }
    for (u32 i = 0; i < d.count && st.ok(); ++i) {
        ArrayLayer& l  = d.layers[i];
        Input const in = layer_input(s, l);
        Source src;
        KtxLevels k;
        st = open_source(ctx, s, in, src);
        if (st.ok()) st = read_ktx2_levels(ctx, s, in.name, src, TextureShape::Tex2D, i, &k);
        src.close();
        if (st.ok() && i > 0) st = check_layer(s, i, in.name, first.desc, k.desc);
        if (st.failed()) {
            k.release(ctx->alloc);
            if (s.out.diag != kDiagArrayLayerMismatch) note_layer(s.out.capture, i, in.name);
            break;
        }
        if (i == 0) firstLevel = first_level(k.desc, s.in.maxExtent);
        l.srcLevels = k.levels - firstLevel;
        l.src       = alloc_array<u64>(ctx->alloc, usize(l.srcLevels) * 2, Tag::Payload);
        std::memcpy(l.src, k.cols + firstLevel, sizeof(u64) * l.srcLevels);
        std::memcpy(l.src + l.srcLevels, k.cols + k.levels + firstLevel, sizeof(u64) * l.srcLevels);
        l.zstd = k.zstd;
        if (i == 0)
            first = k; // its texel lengths go into the layout
        else
            k.release(ctx->alloc);
    }
    if (st.ok()) {
        u32 const levels  = first.levels - firstLevel;
        MetaSet& m        = s.out.next;
        m.texDesc         = first.desc;
        m.texDesc.layers  = d.count;
        m.texDesc.faces   = 1;
        m.texDesc.isArray = true;
        m.layout          = alloc_array<u64>(ctx->alloc, usize(levels) * kLayoutColumns, Tag::Payload);
        m.layoutLevels    = levels;
        m.texFirstLevel   = firstLevel;
        m.texExtent       = s.in.maxExtent;
        std::memset(m.layout + 2 * levels, 0, sizeof(u64) * levels * 2); // per layer: ArrayLayer::src
        std::memcpy(m.layout + 4 * levels, first.cols + 2 * first.levels + firstLevel, sizeof(u64) * levels);
        m.uploadSize = texture_layout(resident_desc(m.texDesc, firstLevel), ctx->cc.optimalRowPitchAlign,
                                      ctx->cc.optimalOffsetAlign, m.layout, m.layout + levels);
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

/// Two steps: the provider's work, then the metadata unit, which opens, reads and validates.
CompletionKind run_meta(Context* ctx, Slot& s) {
    if (s.array) return run_array_meta(ctx, s);
    Input const in = slot_input(s);
    if (prepare_input(ctx, s, in).failed()) return CompletionKind::Failed;
    Source src;
    if (open_source(ctx, s, in, src).failed()) return CompletionKind::Failed;
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

/// Makes `v` hold `n` bytes for the caller to overwrite. No zero fill, and the old bytes are gone.
Span<u8> scratch_bytes(Vec<u8>& v, usize n) {
    v.clear();
    return v.append_uninit(n);
}

/// The buffers of the upload jobs that one run_jobs() call runs. Fresh memory costs a page fault per
/// page, so the jobs reuse them. `reads` and `encoded` are lent to the attempt while its job runs
/// (JobOutput); the decoded bytes and the Zstd context stay with the worker.
struct JobScratch {
    explicit JobScratch(Allocator const* a)
        : reads(a, Tag::Io), encoded(a, Tag::Io), texels(a, Tag::Io), zstd(a) {}
    Vec<ReadRange> reads;
    Vec<u8> encoded;
    Vec<u8> texels;
    fmt::ZstdDecoder zstd;

    void lend(JobOutput& out) {
        std::swap(reads, out.reads);
        std::swap(encoded, out.encoded);
    }
    /// After a job: takes the lent storage back. A buffer that one large asset grew goes to the
    /// allocator, so a worker keeps at most 2 * kKeepBytes between jobs.
    void take_back(JobOutput& out) {
        lend(out);
        if (encoded.capacity() > kKeepBytes) encoded.release();
        if (texels.capacity() > kKeepBytes) texels.release();
    }
    static constexpr usize kKeepBytes = usize(32) << 20;
};

// The upload stage handles one input at a time: the slot's own asset, or one layer of an array.
// plan_input() lists the input's reads, read_input() fills them, decode_input() turns the bytes into
// the upload target. Only s.out carries state from one step to the next.

Status plan_mesh(Slot& s, bool memory, u8* dst) {
    mesh::MeshView const& v   = s.out.next.meshView;
    mesh::FileHeader const& h = v.header();
    if (memory) return kOk;
    if (v.payload_raw()) {
        // Identity layout: one read of GPUD straight into the adapter's memory.
        if (h.payloadDecodedSize > h.gpuDataSize) {
            DiagSink const sink{&capture_fn, &s.out.capture};
            return diagf(&sink, make_status(Code::Corrupt), mesh::kDiagPayloadRaw, Severity::Error,
                         path_of(s), "header", "raw payload larger than GPUD");
        }
        s.out.reads.push_back({h.gpuDataOffset, h.payloadDecodedSize, dst});
        return kOk;
    }
    u8* const enc = scratch_bytes(s.out.encoded, usize(h.gpuDataSize)).data;
    s.out.reads.push_back({h.gpuDataOffset, h.gpuDataSize, enc});
    return kOk;
}

Status decode_mesh(Context* ctx, Slot& s, Source const& mem, u8* dst, JobScratch& sc) {
    DiagSink const sink{&capture_fn, &s.out.capture};
    StrView const name        = path_of(s);
    mesh::MeshView const& v   = s.out.next.meshView;
    mesh::FileHeader const& h = v.header();
    Span<u8> const out(dst, usize(h.payloadDecodedSize));
    if (!mem.memory && v.payload_raw()) return kOk; // read in place
    Span<u8 const> gpud = s.out.encoded.span();
    if (mem.memory) {
        if (h.gpuDataOffset > mem.size || h.gpuDataSize > mem.size - h.gpuDataOffset)
            return make_status(Code::IoEof);
        gpud = mem.mem.subspan(usize(h.gpuDataOffset), usize(h.gpuDataSize));
    }
    mesh::DecodeOptions const dopt{.alloc = ctx->alloc};
    if (v.payload_raw()) {
        ProfileZone const zone(ctx->prof, "kiln.copy", name);
        return mesh::decode_payload(v, gpud, out, dopt, &sink, nullptr, name);
    }
    // Never straight into the adapter's memory: Zstd reads its output back (as in decode_level).
    Vec<u8>& decoded = sc.texels;
    (void)scratch_bytes(decoded, out.size);
    {
        ProfileZone const zone(ctx->prof, "kiln.decode", name);
        KILN_TRY(mesh::decode_payload(v, gpud, decoded.span(), dopt, &sink, nullptr, name));
    }
    ProfileZone const zone(ctx->prof, "kiln.copy", name);
    if (out.size) std::memcpy(out.data, decoded.data(), out.size);
    return kOk;
}

/// One stored level of one input: [sOff, sOff + sLen) in its file, `tLen` texel bytes once decoded,
/// written to `out` with rows of `rowBytes` padded to `pitch`.
struct LevelCopy {
    u32 level    = 0;
    u64 sOff     = 0;
    u64 sLen     = 0;
    u64 tLen     = 0;
    u64 rowBytes = 0;
    u64 pitch    = 0;
    bool zstd    = false;
    u8* out      = nullptr;
    /// The stored bytes are the level as the target holds it: a read can go straight to `out`.
    bool direct() const { return pitch == rowBytes && !zstd; }
};

/// Level `i` of texture input `j` (0 for a texture of its own, else the array layer). Layer j of a
/// level lies at the level's offset plus j times one layer's padded size.
LevelCopy level_copy(Slot& s, u32 j, u32 i, u8* dst) {
    MetaSet const& m = s.out.next;
    u32 const levels = m.layoutLevels;
    LevelCopy c{.level = i,
                .tLen  = m.layout[4 * levels + i],
                .rowBytes =
                    format_row_bytes(m.texDesc.format, max(m.texDesc.width >> (m.texFirstLevel + i), 1u)),
                .pitch = m.layout[levels + i]};
    if (s.array) {
        ArrayLayer const& l = s.array->layers[j];
        c.sOff              = l.src[i];
        c.sLen              = l.src[l.srcLevels + i];
        c.zstd              = l.zstd;
    } else {
        c.sOff = m.layout[2 * levels + i];
        c.sLen = m.layout[3 * levels + i];
        c.zstd = m.texZstd;
    }
    u64 const rows = c.rowBytes ? c.tLen / c.rowBytes : 0;
    c.out          = dst + m.layout[i] + j * c.pitch * rows;
    return c;
}

/// One read per level, in level order: straight to the target where the level allows it, else into
/// `encoded`, the levels back to back.
void plan_texture(Slot& s, u32 j, u8* dst) {
    u32 const levels = s.out.next.layoutLevels;
    u64 total        = 0;
    for (u32 i = 0; i < levels; ++i)
        if (LevelCopy const c = level_copy(s, j, i, dst); !c.direct()) total += c.sLen;
    u8* enc = scratch_bytes(s.out.encoded, usize(total)).data;
    for (u32 i = 0; i < levels; ++i) {
        LevelCopy const c = level_copy(s, j, i, dst);
        s.out.reads.push_back({c.sOff, c.sLen, c.direct() ? c.out : enc});
        if (!c.direct()) enc += c.sLen;
    }
}

Status decode_level(Context* ctx, Slot& s, StrView input, Source const& mem, LevelCopy const& c,
                    JobScratch& sc) {
    u8 const* from = nullptr;
    if (mem.memory) {
        if (c.sOff > mem.size || c.sLen > mem.size - c.sOff) return make_status(Code::IoEof);
        from = mem.mem.data + c.sOff;
    } else {
        from = s.out.reads[c.level].dst;
        if (from == c.out) return kOk; // read in place
    }
    if (c.zstd) {
        // Never straight into the adapter's memory: staging is often write-combined, and Zstd reads
        // its output back for matches (cook-tracing.md, "First findings").
        (void)scratch_bytes(sc.texels, usize(c.tLen));
        ProfileZone const zone(ctx->prof, "kiln.decode", input);
        if (!sc.zstd.decode(Span<u8 const>(from, usize(c.sLen)), sc.texels.span())) {
            DiagSink const sink{&capture_fn, &s.out.capture};
            return diagf(&sink, make_status(Code::Corrupt), ktx2::kDiagKtxLevelDecode, Severity::Error, input,
                         "levelIndex", "level %u does not decode: %s", c.level, sc.zstd.error());
        }
        from = sc.texels.data();
    }
    ProfileZone const zone(ctx->prof, "kiln.copy", input);
    if (c.pitch == c.rowBytes) {
        std::memcpy(c.out, from, usize(c.tLen));
        return kOk;
    }
    u64 const rows = c.rowBytes ? c.tLen / c.rowBytes : 0;
    for (u64 r = 0; r < rows; ++r) {
        u8* row = c.out + r * c.pitch;
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
        u64 const dOff = m.layout[i];
        u64 const rowBytes =
            format_row_bytes(m.texDesc.format, max(m.texDesc.width >> (m.texFirstLevel + i), 1u));
        u64 const rows = rowBytes ? m.layout[4 * levels + i] / rowBytes : 0;
        if (dOff > cursor) std::memset(dst + cursor, 0, usize(dOff - cursor));
        cursor = dOff + m.layout[levels + i] * rows * layers;
    }
    if (cursor < m.uploadSize) std::memset(dst + cursor, 0, usize(m.uploadSize - cursor));
}

/// Lists the reads of input `j` in s.out.reads and sizes s.out.encoded for them. A memory source
/// needs none.
Status plan_input(Slot& s, Input const& in, u32 j, u8* dst) {
    s.out.reads.clear();
    s.out.encoded.clear();
    Source mem;
    bool const memory = memory_source(in, mem);
    if (s.kind == AssetKind::Mesh) return plan_mesh(s, memory, dst);
    if (!memory) plan_texture(s, j, dst);
    return kOk;
}

/// Opens the input's artifact, reads the planned ranges and closes it. This is the part of a load
/// that waits for storage.
Status read_input(Context* ctx, Slot& s, Input const& in) {
    if (s.out.reads.empty()) return kOk;
    Source src;
    KILN_TRY(open_source(ctx, s, in, src));
    // In file order, which helps the system's read-ahead: KTX2 stores the smallest level first.
    usize const n      = s.out.reads.size();
    bool const reverse = s.out.reads[n - 1].offset < s.out.reads[0].offset;
    for (usize i = 0; i < n; ++i) {
        ReadRange const& r = s.out.reads[reverse ? n - 1 - i : i];
        IoBytes budget(ctx, r.size);
        KILN_TRY(src.read(r.offset, r.size, r.dst));
    }
    return kOk;
}

/// Decodes or repacks what read_input() left in s.out.encoded, or the memory source, into the target.
Status decode_input(Context* ctx, Slot& s, Input const& in, u32 j, u8* dst, JobScratch& sc) {
    Source mem;
    (void)memory_source(in, mem);
    if (s.kind == AssetKind::Mesh) return decode_mesh(ctx, s, mem, dst, sc);
    for (u32 i = 0; i < s.out.next.layoutLevels; ++i)
        KILN_TRY(decode_level(ctx, s, in.name, mem, level_copy(s, j, i, dst), sc));
    return kOk;
}

CompletionKind run_upload(Context* ctx, Slot& s, JobScratch& sc) {
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
        ktx2::TextureDesc const d = resident_desc(m.texDesc, m.texFirstLevel);
        td.format                 = d.format;
        td.width                  = d.width;
        td.height                 = d.height;
        td.depth                  = d.depth;
        td.layers                 = d.layers * d.faces; // cube faces are layers at the boundary
        td.shape                  = s.texShape;
        td.levels                 = d.levels;
        td.firstLevel             = m.texFirstLevel;
        ud.kind                   = UploadKind::TextureLevels;
        ud.alignment              = u32(max<u64>(ctx->cc.optimalOffsetAlign, 16));
        ud.texture                = &td;
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
    u8* const dst    = static_cast<u8*>(t.dst);
    u32 const inputs = s.array ? s.array->count : 1;
    if (st.ok() && s.kind == AssetKind::Texture) zero_level_gaps(m, inputs, dst);
    for (u32 j = 0; j < inputs && st.ok(); ++j) {
        Input const in = s.array ? layer_input(s, s.array->layers[j]) : slot_input(s);
        st             = plan_input(s, in, j, dst);
        if (st.ok()) st = read_input(ctx, s, in);
        if (st.ok()) st = decode_input(ctx, s, in, j, dst, sc);
        if (st.failed() && s.array) note_layer(s.out.capture, j, in.name);
    }
    // An artifact that vanished since the meta stage is a failed load, not a miss.
    if (st.failed() && s.out.diag) diag = s.out.diag == kDiagStoreMiss ? kDiagAssetLoadFailed : s.out.diag;
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

namespace {

void ready_link(Context* ctx, Slot& s, ReadyId id) {
    List& l = ctx->ready[u32(id)];
    s.ready = id;
    s.rNext = kInvalid;
    s.rPrev = l.tail;
    if (l.tail != kInvalid)
        ctx->slots[l.tail].rNext = s.index;
    else
        l.head = s.index;
    l.tail = s.index;
    ++l.count;
}

void ready_unlink(Context* ctx, Slot& s) {
    List& l = ctx->ready[u32(s.ready)];
    if (s.rPrev != kInvalid)
        ctx->slots[s.rPrev].rNext = s.rNext;
    else
        l.head = s.rNext;
    if (s.rNext != kInvalid)
        ctx->slots[s.rNext].rPrev = s.rPrev;
    else
        l.tail = s.rPrev;
    --l.count;
    s.ready = ReadyId::None;
    s.rPrev = s.rNext = kInvalid;
}

} // namespace

bool ready_push(Context* ctx, Slot& s, Stage stage) {
    bool const high  = s.priority == Priority::High;
    ReadyId const id = stage == Stage::Meta ? (high ? ReadyId::MetaHigh : ReadyId::MetaNormal)
                                            : (high ? ReadyId::UploadHigh : ReadyId::UploadNormal);
    std::lock_guard<std::mutex> const lock(ctx->readyMutex);
    ready_link(ctx, s, id);
    if (ctx->runners >= ctx->maxIoJobs) return false;
    ++ctx->runners;
    return true;
}

bool ready_remove(Context* ctx, Slot& s) {
    std::lock_guard<std::mutex> const lock(ctx->readyMutex);
    if (s.ready == ReadyId::None) return false;
    ready_unlink(ctx, s);
    return true;
}

void ready_boost(Context* ctx, Slot& s) {
    std::lock_guard<std::mutex> const lock(ctx->readyMutex);
    if (s.ready != ReadyId::MetaNormal && s.ready != ReadyId::UploadNormal) return;
    ReadyId const to = s.ready == ReadyId::MetaNormal ? ReadyId::MetaHigh : ReadyId::UploadHigh;
    ready_unlink(ctx, s);
    ready_link(ctx, s, to);
}

namespace {

void run_job(Context* ctx, Slot& s, JobScratch& sc) {
    s.out.status = kOk;
    s.out.diag   = 0;
    s.out.capture.reset();
    bool const meta = s.in.stage == Stage::Meta;
    CompletionKind k;
    {
        if (ctx->prof)
            profile_interval(ctx->prof, "kiln.wait.pool", path_of(s), s.submitNs, profile_now_ns());
        ProfileZone const zone(ctx->prof, meta ? "kiln.meta" : "kiln.upload", path_of(s));
        if (meta) {
            k = run_meta(ctx, s);
        } else {
            sc.lend(s.out);
            k = run_upload(ctx, s, sc);
            sc.take_back(s.out);
        }
    }
    Completion const c{s.index, s.in.gen, k};
    post(ctx, c); // from here on the pump thread may reuse `s`
}

} // namespace

void run_jobs(void* arg) {
    Context* ctx = static_cast<Context*>(arg);
    JobScratch sc(ctx->alloc); // freed when no job is left
    for (;;) {
        Slot* s = nullptr;
        {
            std::lock_guard<std::mutex> const lock(ctx->readyMutex);
            for (List const& l : ctx->ready) {
                if (l.head == kInvalid) continue;
                s = &ctx->slots[l.head];
                ready_unlink(ctx, *s);
                break;
            }
            if (!s) --ctx->runners; // under the lock, so a push after it submits a new run_jobs()
        }
        if (!s) break;
        run_job(ctx, *s, sc);
    }
    ctx->jobsInFlight.fetch_sub(1, std::memory_order_acq_rel); // the last access to the context
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
