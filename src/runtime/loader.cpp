// loader.cpp — job side: the steps of the meta and upload stages, and the lanes that run them.
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

u32 input_count(Slot const& s) { return s.array ? s.array->count : 1; }

Input input_of(Slot& s, u32 j) { return s.array ? layer_input(s, s.array->layers[j]) : slot_input(s); }

/// What a step leaves: the stage's result, or the next step in s.out.step.
struct StepOut {
    bool done           = false;
    CompletionKind kind = CompletionKind::Failed;
};

StepOut done(CompletionKind kind) { return {true, kind}; }

StepOut next(Slot& s, JobStep step) {
    s.out.step = step;
    return {};
}

/// The provider's work for every input.
StepOut prepare_step(Context* ctx, Slot& s) {
    for (u32 i = 0; i < input_count(s); ++i) {
        Input const in = input_of(s, i);
        if (prepare_input(ctx, s, in).ok()) continue;
        if (s.array) note_layer(s.out.capture, i, in.name);
        return done(CompletionKind::Failed);
    }
    return next(s, JobStep::Metadata);
}

/// Each layer's metadata, checked against layer 0, then the upload layout of the whole array. Each
/// layer keeps its level table for the upload stage.
Status array_metadata(Context* ctx, Slot& s) {
    ArrayDecl& d = *s.array;
    KtxLevels first;
    Status st = kOk;
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
    return st;
}

/// The metadata unit: opens each input, reads and validates its metadata, and closes it.
StepOut metadata_step(Context* ctx, Slot& s) {
    Status st = kOk;
    if (s.array) {
        st = array_metadata(ctx, s);
    } else {
        Source src;
        st = open_source(ctx, s, slot_input(s), src);
        if (st.ok()) st = s.kind == AssetKind::Mesh ? mesh_meta(ctx, s, src) : texture_meta(ctx, s, src);
    }
    if (st.failed()) {
        s.out.status = st;
        if (s.out.diag == 0) s.out.diag = kDiagAssetLoadFailed;
        return done(CompletionKind::Failed);
    }
    return done(CompletionKind::MetaReady);
}

// ---------------------------------------------------------------------------
// Upload stage
// ---------------------------------------------------------------------------

/// Makes `v` hold `n` bytes for the caller to overwrite. No zero fill, and the old bytes are gone.
Span<u8> scratch_bytes(Vec<u8>& v, usize n) {
    v.clear();
    return v.append_uninit(n);
}

/// A buffer above this size goes back to the allocator after its stage, and a chunk of array layers
/// is planned to stay below it.
constexpr usize kKeepBytes = usize(32) << 20;

/// What one run_jobs() call keeps for the decodes it runs: the decoded bytes and the Zstd context.
/// Fresh memory costs a page fault per page, so the jobs reuse them.
struct JobScratch {
    explicit JobScratch(Allocator const* a) : texels(a, Tag::Io), zstd(a) {}
    Vec<u8> texels;
    fmt::ZstdDecoder zstd;

    void trim() {
        if (texels.capacity() > kKeepBytes) texels.release();
    }
};

/// Gives the stage its read storage: a buffer an earlier stage used, when one is left.
void buf_take(Context* ctx, JobOutput& out) {
    out.bufTaken = true;
    {
        std::lock_guard<std::mutex> const lock(ctx->bufMutex);
        if (ctx->bufCount) {
            ReadBuf& b  = ctx->bufs[--ctx->bufCount];
            out.reads   = std::move(b.reads);
            out.encoded = std::move(b.encoded);
            return;
        }
    }
    out.reads   = Vec<ReadRange>(ctx->alloc, Tag::Io);
    out.encoded = Vec<u8>(ctx->alloc, Tag::Io);
}

void buf_give(Context* ctx, JobOutput& out) {
    out.bufTaken = false;
    if (out.encoded.capacity() <= kKeepBytes) {
        std::lock_guard<std::mutex> const lock(ctx->bufMutex);
        if (ctx->bufCount < ctx->bufCap) {
            ReadBuf& b = ctx->bufs[ctx->bufCount++];
            b.reads    = std::move(out.reads);
            b.encoded  = std::move(out.encoded);
            return;
        }
    }
    out.reads.release();
    out.encoded.release();
}

/// No job is left: the buffers go back to the allocator.
void bufs_free(Context* ctx) {
    std::lock_guard<std::mutex> const lock(ctx->bufMutex);
    for (u32 i = 0; i < ctx->bufCount; ++i) {
        ctx->bufs[i].reads.release();
        ctx->bufs[i].encoded.release();
    }
    ctx->bufCount = 0;
}

// The upload stage reads a chunk of inputs, then decodes it: the slot's own asset, or some layers of
// an array. read_step() lists the reads and fills them, decode_step() turns the bytes into the upload
// target. Only s.out carries state from one step to the next.

/// The bytes of `encoded` that the reads of a mesh need.
u64 mesh_encoded_size(Slot const& s) {
    mesh::MeshView const& v = s.out.next.meshView;
    return v.payload_raw() ? 0 : v.header().gpuDataSize;
}

Status plan_mesh(Slot& s, u8* dst, u8*& enc) {
    mesh::MeshView const& v   = s.out.next.meshView;
    mesh::FileHeader const& h = v.header();
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
    s.out.reads.push_back({h.gpuDataOffset, h.gpuDataSize, enc});
    enc += h.gpuDataSize;
    return kOk;
}

/// `read` is the mesh's one read; a memory source has none.
Status decode_mesh(Context* ctx, Slot& s, Source const& mem, ReadRange const* read, u8* dst, JobScratch& sc) {
    DiagSink const sink{&capture_fn, &s.out.capture};
    StrView const name        = path_of(s);
    mesh::MeshView const& v   = s.out.next.meshView;
    mesh::FileHeader const& h = v.header();
    Span<u8> const out(dst, usize(h.payloadDecodedSize));
    if (read && v.payload_raw()) return kOk; // read in place
    Span<u8 const> gpud;
    if (read) {
        gpud = Span<u8 const>(read->dst, usize(read->size));
    } else {
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
LevelCopy level_copy(Slot const& s, u32 j, u32 i) {
    MetaSet const& m = s.out.next;
    u32 const levels = m.layoutLevels;
    LevelCopy c{.level    = i,
                .tLen     = m.layout[4 * levels + i],
                .rowBytes = format_row_bytes(m.texDesc.format, max(m.texDesc.width >> i, 1u)),
                .pitch    = m.layout[levels + i]};
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
    return c;
}

/// level_copy() with the level's place in the target at `dst`.
LevelCopy level_at(Slot const& s, u32 j, u32 i, u8* dst) {
    LevelCopy c    = level_copy(s, j, i);
    u64 const rows = c.rowBytes ? c.tLen / c.rowBytes : 0;
    c.out          = dst + s.out.next.layout[i] + j * c.pitch * rows;
    return c;
}

/// The bytes of `encoded` that the reads of texture input `j` need.
u64 texture_encoded_size(Slot const& s, u32 j) {
    u64 total = 0;
    for (u32 i = 0; i < s.out.next.layoutLevels; ++i)
        if (LevelCopy const c = level_copy(s, j, i); !c.direct()) total += c.sLen;
    return total;
}

/// One read per level, in level order: straight to the target where the level allows it, else into
/// `encoded` at `enc`, the levels back to back.
void plan_texture(Slot& s, u32 j, u8* dst, u8*& enc) {
    for (u32 i = 0; i < s.out.next.layoutLevels; ++i) {
        LevelCopy const c = level_at(s, j, i, dst);
        s.out.reads.push_back({c.sOff, c.sLen, c.direct() ? c.out : enc});
        if (!c.direct()) enc += c.sLen;
    }
}

/// `read` is the level's read; a memory source has none.
Status decode_level(Context* ctx, Slot& s, StrView input, Source const& mem, ReadRange const* read,
                    LevelCopy const& c, JobScratch& sc) {
    u8 const* from = nullptr;
    if (read) {
        from = read->dst;
        if (from == c.out) return kOk; // read in place
    } else {
        if (c.sOff > mem.size || c.sLen > mem.size - c.sOff) return make_status(Code::IoEof);
        from = mem.mem.data + c.sOff;
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
        u64 const dOff     = m.layout[i];
        u64 const rowBytes = format_row_bytes(m.texDesc.format, max(m.texDesc.width >> i, 1u));
        u64 const rows     = rowBytes ? m.layout[4 * levels + i] / rowBytes : 0;
        if (dOff > cursor) std::memset(dst + cursor, 0, usize(dOff - cursor));
        cursor = dOff + m.layout[levels + i] * rows * layers;
    }
    if (cursor < m.uploadSize) std::memset(dst + cursor, 0, usize(m.uploadSize - cursor));
}

/// Opens the input's artifact, reads its planned ranges (s.out.reads from `first` on) and closes it.
/// This is the part of a load that waits for storage.
Status read_input(Context* ctx, Slot& s, Input const& in, usize first) {
    usize const n = s.out.reads.size() - first;
    if (n == 0) return kOk;
    ReadRange const* const reads = s.out.reads.data() + first;
    Source src;
    KILN_TRY(open_source(ctx, s, in, src));
    // In file order, which helps the system's read-ahead: KTX2 stores the smallest level first.
    bool const reverse = reads[n - 1].offset < reads[0].offset;
    for (usize i = 0; i < n; ++i) {
        ReadRange const& r = reads[reverse ? n - 1 - i : i];
        IoBytes budget(ctx, r.size);
        KILN_TRY(src.read(r.offset, r.size, r.dst));
    }
    return kOk;
}

/// Ends the upload stage: the target is committed, or discarded after a failure.
CompletionKind end_upload(Context* ctx, Slot& s, Status st, u32 diag = kDiagAssetLoadFailed) {
    Adapter const& a = ctx->adapter;
    if (s.out.hasTarget && st.failed() && a.discard_upload) {
        a.discard_upload(a.user,
                         s.out.target.token); // nothing for the GPU: the adapter frees ticket and object
        s.out.hasTarget = false;
    } else if (s.out.hasTarget) {
        a.commit_upload(a.user, s.out.target.token); // a failed load commits too; kiln destroys the result
    }
    if (st.ok()) return CompletionKind::Uploaded;
    // An artifact that vanished since the meta stage is a failed load, not a miss.
    if (s.out.diag) diag = s.out.diag == kDiagStoreMiss ? kDiagAssetLoadFailed : s.out.diag;
    s.out.status = st;
    s.out.diag   = diag;
    return CompletionKind::Failed;
}

/// Asks the adapter for the upload target. Not done: the target is in s.out.target.
StepOut begin_target(Context* ctx, Slot& s) {
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
    Status const st = a.begin_upload(a.user, ud, &t);
    if (st.code == Code::Busy) return done(CompletionKind::BusyRetry);
    if (st.failed()) {
        note(s.out.capture, "begin_upload failed (%llu bytes)", static_cast<unsigned long long>(ud.size));
        s.out.status = st;
        s.out.diag   = kDiagAdapterRejected;
        return done(CompletionKind::Failed);
    }
    s.out.target    = t;
    s.out.hasTarget = true;

    if (!t.dst && ud.size) {
        note(s.out.capture, "begin_upload returned no destination memory");
        return done(end_upload(ctx, s, make_status(Code::Internal), kDiagAdapterRejected));
    }
    for (u32 i = 0; s.kind == AssetKind::Texture && t.rowPitchAlign > 1 && i < m.layoutLevels; ++i) {
        if (m.layout[m.layoutLevels + i] % t.rowPitchAlign == 0) continue;
        note(s.out.capture, "adapter row pitch alignment %llu differs from copy_constraints (%llu)",
             static_cast<unsigned long long>(t.rowPitchAlign),
             static_cast<unsigned long long>(ctx->cc.optimalRowPitchAlign));
        return done(end_upload(ctx, s, make_status(Code::Unsupported), kDiagAdapterRejected));
    }
    if (s.kind == AssetKind::Texture) zero_level_gaps(m, input_count(s), static_cast<u8*>(t.dst));
    return {};
}

/// Begins the upload on the stage's first call. Then takes the next inputs, as many as keep `encoded`
/// below kKeepBytes and at least one, and reads them.
StepOut read_step(Context* ctx, Slot& s) {
    if (!s.out.hasTarget)
        if (StepOut const o = begin_target(ctx, s); o.done) return o;
    u8* const dst   = static_cast<u8*>(s.out.target.dst);
    u32 const first = s.out.inputEnd;
    u32 end         = first;
    u64 total       = 0;
    for (; end < input_count(s); ++end) {
        Source mem;
        u64 const size = memory_source(input_of(s, end), mem) ? 0
                         : s.kind == AssetKind::Mesh          ? mesh_encoded_size(s)
                                                              : texture_encoded_size(s, end);
        if (end > first && total + size > kKeepBytes) break;
        total += size;
    }
    s.out.inputFirst = first;
    s.out.inputEnd   = end;
    s.out.reads.clear();
    u8* enc = scratch_bytes(s.out.encoded, usize(total)).data;
    for (u32 j = first; j < end; ++j) {
        Input const in = input_of(s, j);
        usize const at = s.out.reads.size();
        Source mem;
        Status st = kOk;
        if (!memory_source(in, mem)) {
            if (s.kind == AssetKind::Mesh)
                st = plan_mesh(s, dst, enc);
            else
                plan_texture(s, j, dst, enc);
        }
        if (st.ok()) st = read_input(ctx, s, in, at);
        if (st.failed()) {
            if (s.array) note_layer(s.out.capture, j, in.name);
            return done(end_upload(ctx, s, st));
        }
    }
    return next(s, JobStep::Decode);
}

/// Decodes or repacks the chunk that read_step() left, or its memory sources, into the target. Then
/// the next chunk is read, or the upload is committed.
StepOut decode_step(Context* ctx, Slot& s, JobScratch& sc) {
    u8* const dst         = static_cast<u8*>(s.out.target.dst);
    ReadRange const* read = s.out.reads.data();
    u32 const levels      = s.out.next.layoutLevels;
    for (u32 j = s.out.inputFirst; j < s.out.inputEnd; ++j) {
        Input const in = input_of(s, j);
        Source mem;
        bool const file = !memory_source(in, mem);
        Status st       = kOk;
        if (s.kind == AssetKind::Mesh) {
            st = decode_mesh(ctx, s, mem, file ? read : nullptr, dst, sc);
            if (file) ++read;
        } else {
            for (u32 i = 0; i < levels && st.ok(); ++i)
                st =
                    decode_level(ctx, s, in.name, mem, file ? read + i : nullptr, level_at(s, j, i, dst), sc);
            if (file) read += levels;
        }
        if (st.failed()) {
            if (s.array) note_layer(s.out.capture, j, in.name);
            return done(end_upload(ctx, s, st));
        }
    }
    if (s.out.inputEnd < input_count(s)) return next(s, JobStep::Read);
    return done(end_upload(ctx, s, kOk));
}

StepOut run_step(Context* ctx, Slot& s, JobScratch& sc) {
    switch (s.out.step) {
    case JobStep::Prepare: return prepare_step(ctx, s);
    case JobStep::Metadata: return metadata_step(ctx, s);
    case JobStep::Read: return read_step(ctx, s);
    case JobStep::Decode: return decode_step(ctx, s, sc);
    }
    return done(CompletionKind::Failed);
}

/// The slot was released, or the context stops: the stage ends without its remaining steps.
CompletionKind abandon(Context* ctx, Slot& s) {
    if (s.in.stage == Stage::Upload) return end_upload(ctx, s, make_status(Code::Cancelled));
    s.out.status = make_status(Code::Cancelled);
    return CompletionKind::Failed;
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

List& ready_list(Context* ctx, Lane lane, ReadyId id) { return ctx->ready[u32(lane)][u32(id)]; }

/// A stage that goes on (`front`) passes the stages that did not start.
void ready_link(Context* ctx, Slot& s, Lane lane, ReadyId id, bool front) {
    List& l = ready_list(ctx, lane, id);
    s.ready = id;
    s.lane  = lane;
    s.rPrev = front ? kInvalid : l.tail;
    s.rNext = front ? l.head : kInvalid;
    if (s.rPrev != kInvalid) ctx->slots[s.rPrev].rNext = s.index;
    if (s.rNext != kInvalid) ctx->slots[s.rNext].rPrev = s.index;
    if (front || l.head == kInvalid) l.head = s.index;
    if (!front || l.tail == kInvalid) l.tail = s.index;
    ++l.count;
}

void ready_unlink(Context* ctx, Slot& s) {
    List& l = ready_list(ctx, s.lane, s.ready);
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

ReadyId ready_id(Slot const& s) {
    return s.in.stage == Stage::Meta ? (s.readyHigh ? ReadyId::MetaHigh : ReadyId::MetaNormal)
                                     : (s.readyHigh ? ReadyId::UploadHigh : ReadyId::UploadNormal);
}

/// Links `s` for its next step. Returns the lane that the caller submits one more run_jobs() call
/// to, or Lane::Count for none: `lane` when it runs fewer calls than it may, else the workers for a
/// read step, because they take those too. `fresh`: the stage starts; else a job hands it over.
Lane ready_add(Context* ctx, Slot& s, Lane lane, bool fresh) {
    std::lock_guard<std::mutex> const lock(ctx->readyMutex);
    if (fresh) {
        s.started   = false;
        s.readyHigh = s.priority == Priority::High;
    }
    ready_link(ctx, s, lane, ready_id(s), !fresh);
    for (Lane const to : {lane, Lane::Cpu}) {
        if (ctx->runners[u32(to)] >= ctx->maxRunners[u32(to)]) continue;
        ++ctx->runners[u32(to)];
        return to;
    }
    return Lane::Count;
}

/// The next job for a run_jobs() call of `lane`, the first ReadyId first. A worker takes read steps
/// too, after its own steps of the same ReadyId: the readers add to the threads that read, they do
/// not replace them.
/// An upload stage that starts needs one of the bufCap places; without one the stages of its list
/// wait, and an upload that ends calls kick(). Null: no job, and the caller's run_jobs() call ends;
/// `idle` then says that no lane runs one.
Slot* ready_pop(Context* ctx, Lane lane, bool& idle) {
    std::lock_guard<std::mutex> const lock(ctx->readyMutex);
    for (u32 id = 0; id < u32(ReadyId::Count); ++id) {
        for (u32 from = u32(lane); from < u32(Lane::Count); ++from) {
            List const& l = ctx->ready[from][id];
            if (l.head == kInvalid) continue;
            Slot& s = ctx->slots[l.head];
            if (s.in.stage == Stage::Upload && !s.out.hasBuf) {
                if (ctx->bufOut >= ctx->bufCap) continue;
                ++ctx->bufOut;
                s.out.hasBuf = true;
            }
            ready_unlink(ctx, s);
            s.started = true;
            return &s;
        }
    }
    --ctx->runners[u32(lane)]; // under the lock, so a push after it submits a new run_jobs()
    idle = ctx->runners[u32(Lane::Cpu)] + ctx->runners[u32(Lane::Read)] == 0;
    return nullptr;
}

/// An upload stage ended and its place is free: a lane with waiting jobs and no run_jobs() call gets one.
void kick(Context* ctx) {
    bool submit[u32(Lane::Count)] = {};
    {
        std::lock_guard<std::mutex> const lock(ctx->readyMutex);
        --ctx->bufOut;
        for (u32 lane = 0; lane < u32(Lane::Count); ++lane) {
            if (ctx->runners[lane] != 0 || ctx->maxRunners[lane] == 0) continue;
            for (List const& l : ctx->ready[lane])
                submit[lane] |= l.head != kInvalid;
            if (submit[lane]) ++ctx->runners[lane];
        }
    }
    for (u32 lane = 0; lane < u32(Lane::Count); ++lane)
        if (submit[lane]) submit_runner(ctx, Lane(lane));
}

void post(Context* ctx, Completion const& c) {
    std::lock_guard<std::mutex> lock(ctx->compMutex);
    KILN_VERIFY(ctx->compCount < ctx->compCap); // one job per slot: never full
    ctx->comp[(ctx->compHead + ctx->compCount) % ctx->compCap] = c;
    ++ctx->compCount;
}

/// Runs the steps of `s` one after another: a worker all of them, a read job those of its lane. A
/// read job's next step for the workers goes to their ready list, with no pump between; the stage's
/// last step posts its completion.
void run_job(Context* ctx, Slot& s, Lane lane, JobScratch& sc) {
    bool const meta = s.in.stage == Stage::Meta;
    if (!meta && !s.out.bufTaken) buf_take(ctx, s.out);
    bool ended       = false;
    CompletionKind k = CompletionKind::Failed;
    {
        if (ctx->prof)
            profile_interval(ctx->prof, "kiln.wait.pool", path_of(s), s.submitNs, profile_now_ns());
        // An upload's steps on the read lane have a zone of their own; "kiln.upload" is then the decode.
        char const* const name = meta ? "kiln.meta" : lane == Lane::Read ? "kiln.upload.read" : "kiln.upload";
        ProfileZone const zone(ctx->prof, name, path_of(s));
        do {
            if (s.in.abandoned.load(std::memory_order_acquire) ||
                ctx->stopping.load(std::memory_order_acquire)) {
                k     = abandon(ctx, s);
                ended = true;
                break;
            }
            StepOut const o = run_step(ctx, s, sc);
            k               = o.kind;
            ended           = o.done;
        } while (!ended && (lane == Lane::Cpu || step_lane(ctx, s) == lane));
    }
    if (!ended) {
        if (ctx->prof) s.submitNs = profile_now_ns();
        Lane const runner =
            ready_add(ctx, s, step_lane(ctx, s), false); // from here on another job may run `s`
        if (runner != Lane::Count) submit_runner(ctx, runner);
        return;
    }
    if (!meta) {
        buf_give(ctx, s.out);
        s.out.hasBuf = false;
        kick(ctx);
    }
    Completion const c{s.index, s.in.gen, k};
    post(ctx, c); // from here on the pump thread may reuse `s`
}

} // namespace

Lane step_lane(Context const* ctx, Slot const& s) {
    if (!ctx->readLane || s.out.step == JobStep::Prepare || s.out.step == JobStep::Decode) return Lane::Cpu;
    // A step that reads no file does not wait for storage.
    if (s.source == SourceKind::Memory) return Lane::Cpu;
    if (!s.array) return s.out.cookedValid ? Lane::Cpu : Lane::Read;
    for (u32 i = 0; i < s.array->count; ++i)
        if (!s.array->layers[i].cookedValid) return Lane::Read;
    return Lane::Cpu;
}

Lane ready_push(Context* ctx, Slot& s, Lane lane) { return ready_add(ctx, s, lane, true); }

void submit_runner(Context* ctx, Lane lane) {
    JobSystem const& js = lane == Lane::Read ? ctx->readJobs : ctx->jobs;
    ctx->jobsInFlight.fetch_add(1, std::memory_order_acq_rel);
    js.submit(js.user, &run_jobs, &ctx->laneRef[u32(lane)]);
}

bool ready_remove(Context* ctx, Slot& s) {
    std::lock_guard<std::mutex> const lock(ctx->readyMutex);
    if (s.ready == ReadyId::None || s.started) return false;
    ready_unlink(ctx, s);
    return true;
}

void ready_drop_unstarted(Context* ctx) {
    std::lock_guard<std::mutex> const lock(ctx->readyMutex);
    for (auto& lane : ctx->ready)
        for (List const& l : lane)
            for (u32 i = l.head; i != kInvalid;) {
                Slot& s        = ctx->slots[i];
                u32 const next = s.rNext;
                if (!s.started) ready_unlink(ctx, s);
                i = next;
            }
}

void ready_boost(Context* ctx, Slot& s) {
    std::lock_guard<std::mutex> const lock(ctx->readyMutex);
    s.readyHigh = true;
    if (s.ready != ReadyId::MetaNormal && s.ready != ReadyId::UploadNormal) return;
    Lane const lane = s.lane;
    ready_unlink(ctx, s);
    ready_link(ctx, s, lane, ready_id(s), s.started);
}

void run_jobs(void* arg) {
    LaneRef const& ref = *static_cast<LaneRef const*>(arg);
    Context* ctx       = ref.ctx;
    JobScratch sc(ctx->alloc); // freed when no job is left
    bool idle = false;
    while (Slot* s = ready_pop(ctx, ref.lane, idle)) {
        run_job(ctx, *s, ref.lane, sc);
        sc.trim();
    }
    if (idle) bufs_free(ctx);
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
