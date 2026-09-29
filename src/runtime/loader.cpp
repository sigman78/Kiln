// loader.cpp — worker side: one job runs the meta or the upload stage of one asset.
// See docs/design/threading-and-io.md.
#include "runtime_internal.h"

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

void note(DiagCapture& c, char const* fmt, ...) noexcept KILN_PRINTF(2, 3);
void note(DiagCapture& c, char const* fmt, ...) noexcept {
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
    IoBytes(Context* ctx, u64 n) noexcept : ctx_(ctx), n_(n) {
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
    ~IoBytes() noexcept {
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

    Source() noexcept                = default;
    Source(Source const&)            = delete;
    Source& operator=(Source const&) = delete;
    ~Source() noexcept { close(); }

    [[nodiscard]] Status read(u64 off, u64 n, void* dst) const noexcept {
        if (off > size || n > size - off) return make_status(Code::IoEof);
        if (n == 0) return kOk;
        if (memory) {
            std::memcpy(dst, mem.data + off, usize(n));
            return kOk;
        }
        return io->read_range(io->user, file, off, n, dst);
    }
    void close() noexcept {
        if (!memory && file.valid()) io->close(io->user, file);
        file = {};
    }
};

Span<u8 const> memory_bytes(Slot const& s) noexcept {
    return s.cookedValid ? s.cooked.span() : s.memory.span();
}

/// Resolve the slot's source. For a store miss with a cook provider (meta stage only)
/// the provider cooks into `s.cooked`, which then serves both stages.
Status open_source(Context* ctx, Slot& s, Source& src, bool allowCook) noexcept {
    if (s.cookedValid || s.source == SourceKind::Memory) {
        src.memory = true;
        src.mem    = memory_bytes(s);
        src.size   = src.mem.size;
        return kOk;
    }
    char file[1024];
    usize const n = store_path(ctx, s.kind, path_of(s), file, sizeof file);
    if (n + 1 >= sizeof file) {
        note(s.capture, "store path too long");
        s.jobDiag          = kDiagAssetLoadFailed;
        return s.jobStatus = make_status(Code::InvalidArgument);
    }

    IoBackend const* io = ctx->io;
    StrView const fileSv(file, n);
    // Stat before open: if the file changes in between, the poller sees one extra change
    // and reloads again, rather than missing the change.
    if (allowCook && ctx->watch) s.jobStatValid = io->stat(io->user, fileSv, &s.jobStat).ok();
    Status st = io->open(io->user, fileSv, &src.file);
    if (st.ok()) {
        src.io = io;
        st     = io->size(io->user, src.file, &src.size);
        if (st.failed()) {
            src.close();
            note(s.capture, "cannot stat '%s'", file);
            s.jobDiag          = kDiagAssetLoadFailed;
            return s.jobStatus = st;
        }
        return kOk;
    }
    if (st.code != Code::NotFound) {
        note(s.capture, "cannot open '%s'", file);
        s.jobDiag          = kDiagAssetLoadFailed;
        return s.jobStatus = st;
    }
    if (!allowCook || !s.provider.cook) {
        note(s.capture, "no cooked file '%s' and no cook provider", file);
        s.jobDiag          = kDiagStoreMiss;
        return s.jobStatus = st;
    }

    // Cook on miss (may take seconds; we are on a worker).
    Vec<u8> out(ctx->alloc, Tag::Payload);
    DiagSink sink{&capture_fn, &s.capture};
    Status const cs = s.provider.cook(s.provider.user, s.kind, path_of(s), ctx->alloc, &out, &sink);
    if (cs.failed()) {
        if (cs.code == Code::NotFound) {
            note(s.capture, "no cooked file '%s' and the cook provider found no source", file);
            s.jobDiag = kDiagStoreMiss;
        } else {
            note(s.capture, "cook provider failed");
            s.jobDiag = kDiagCookOnMissFailed;
        }
        return s.jobStatus = cs;
    }
    // A provider that writes the store (storeMode Disk) leaves a file to watch.
    if (ctx->watch) s.jobStatValid = io->stat(io->user, fileSv, &s.jobStat).ok();
    s.cooked      = std::move(out);
    s.cookedValid = true;
    src.memory    = true;
    src.mem       = s.cooked.span();
    src.size      = src.mem.size;
    return kOk;
}

// ---------------------------------------------------------------------------
// Meta stage
// ---------------------------------------------------------------------------

Status mesh_meta(Context* ctx, Slot& s, Source const& src) noexcept {
    DiagSink const sink{&capture_fn, &s.capture};
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

    MetaSet& m = s.next;
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

Status texture_meta(Context* ctx, Slot& s, Source const& src) noexcept {
    DiagSink const sink{&capture_fn, &s.capture};
    StrView const name = path_of(s);
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
    if (TextureShape const shape = shape_of(d); shape != s.texShape) {
        prefix.release();
        note(s.capture, "the cooked texture is %s, the request expects %s", texture_shape_name(shape),
             texture_shape_name(s.texShape));
        s.jobDiag = kDiagTextureShapeMismatch;
        return make_status(Code::ValidationFailed);
    }
    u32 const levels = d.levels;
    u64* layout      = alloc_array<u64>(ctx->alloc, usize(levels) * 4, Tag::Payload);
    MetaSet& m       = s.next;
    m.layout         = layout;
    m.layoutLevels   = levels;
    m.uploadSize =
        texture_layout(d, ctx->cc.optimalRowPitchAlign, ctx->cc.optimalOffsetAlign, layout, layout + levels);
    FormatInfo const& fi = v.info();
    for (u32 i = 0; i < levels && st.ok(); ++i) {
        ktx2::LevelIndex const& li = v.levels()[i];
        layout[2 * levels + i]     = li.byteOffset;
        layout[3 * levels + i]     = li.byteLength;
        u64 const rowBytes         = format_row_bytes(d.format, v.level_width(i));
        u64 const rows = (u64(v.level_height(i)) + fi.blockHeight - 1) / fi.blockHeight * v.level_depth(i) *
                         d.layers * d.faces;
        if (li.byteLength != rowBytes * rows)
            st = diagf(&sink, make_status(Code::Corrupt), ktx2::kDiagKtxLevelIndex, Severity::Error, name,
                       "levelIndex", "level %u holds %llu bytes, expected %llu", i,
                       static_cast<unsigned long long>(li.byteLength),
                       static_cast<unsigned long long>(rowBytes * rows));
        else if (li.byteOffset > src.size || li.byteLength > src.size - li.byteOffset)
            st = diagf(&sink, make_status(Code::Corrupt), ktx2::kDiagKtxLevelIndex, Severity::Error, name,
                       "levelIndex", "level %u data lies outside the file (%llu bytes)", i,
                       static_cast<unsigned long long>(src.size));
    }
    m.texDesc = d;
    prefix.release();
    return st;
}

CompletionKind run_meta(Context* ctx, Slot& s) noexcept {
    s.jobStatValid = false;
    Source src;
    if (open_source(ctx, s, src, true).failed()) return CompletionKind::Failed;
    Status const st = s.kind == AssetKind::Mesh ? mesh_meta(ctx, s, src) : texture_meta(ctx, s, src);
    src.close();
    if (st.failed()) {
        s.jobStatus = st;
        if (s.jobDiag == 0) s.jobDiag = kDiagAssetLoadFailed;
        return CompletionKind::Failed;
    }
    return CompletionKind::MetaReady;
}

// ---------------------------------------------------------------------------
// Upload stage
// ---------------------------------------------------------------------------

Status write_mesh(Context* ctx, Slot& s, Source const& src, u8* dst) noexcept {
    DiagSink const sink{&capture_fn, &s.capture};
    StrView const name        = path_of(s);
    mesh::MeshView const& v   = s.next.meshView;
    mesh::FileHeader const& h = v.header();
    Span<u8> const out(dst, usize(h.payloadDecodedSize));
    if (h.gpuDataOffset > src.size || h.gpuDataSize > src.size - h.gpuDataOffset)
        return make_status(Code::IoEof);
    if (src.memory)
        return mesh::decode_payload(v, src.mem.subspan(usize(h.gpuDataOffset), usize(h.gpuDataSize)), out, {},
                                    &sink, nullptr, name);
    if (v.payload_raw()) {
        // Identity layout: one read of GPUD straight into the adapter's memory.
        if (h.payloadDecodedSize > h.gpuDataSize)
            return diagf(&sink, make_status(Code::Corrupt), mesh::kDiagPayloadRaw, Severity::Error, name,
                         "header", "raw payload larger than GPUD");
        IoBytes budget(ctx, h.payloadDecodedSize);
        return src.read(h.gpuDataOffset, h.payloadDecodedSize, dst);
    }
    Vec<u8> scratch(ctx->alloc, Tag::Io);
    scratch.resize(usize(h.gpuDataSize));
    {
        IoBytes budget(ctx, h.gpuDataSize);
        KILN_TRY(src.read(h.gpuDataOffset, h.gpuDataSize, scratch.data()));
    }
    return mesh::decode_payload(v, scratch.span(), out, {}, &sink, nullptr, name);
}

Status write_texture(Context* ctx, Slot& s, Source const& src, u8* dst) noexcept {
    MetaSet const& m           = s.next;
    ktx2::TextureDesc const& d = m.texDesc;
    u32 const levels           = m.layoutLevels;
    u64 const* layout          = m.layout;
    Vec<u8> scratch(ctx->alloc, Tag::Io);
    u64 cursor = 0;
    for (u32 i = 0; i < levels; ++i) {
        u64 const dOff  = layout[i];
        u64 const pitch = layout[levels + i];
        u64 const sOff  = layout[2 * levels + i];
        u64 const sLen  = layout[3 * levels + i];
        if (dOff > cursor) std::memset(dst + cursor, 0, usize(dOff - cursor));
        u64 const rowBytes = format_row_bytes(d.format, max(d.width >> i, 1u));
        u64 const rows     = rowBytes ? sLen / rowBytes : 0;
        if (pitch == rowBytes) {
            IoBytes budget(ctx, sLen);
            KILN_TRY(src.read(sOff, sLen, dst + dOff));
        } else {
            u8 const* from = nullptr;
            if (src.memory) {
                if (sOff > src.size || sLen > src.size - sOff) return make_status(Code::IoEof);
                from = src.mem.data + sOff;
            } else {
                scratch.resize(usize(sLen));
                IoBytes budget(ctx, sLen);
                KILN_TRY(src.read(sOff, sLen, scratch.data()));
                from = scratch.data();
            }
            for (u64 r = 0; r < rows; ++r) {
                u8* row = dst + dOff + r * pitch;
                std::memcpy(row, from + r * rowBytes, usize(rowBytes));
                std::memset(row + rowBytes, 0, usize(pitch - rowBytes));
            }
        }
        cursor = dOff + pitch * rows;
    }
    if (cursor < m.uploadSize) std::memset(dst + cursor, 0, usize(m.uploadSize - cursor));
    return kOk;
}

CompletionKind run_upload(Context* ctx, Slot& s) noexcept {
    Adapter const& a = ctx->adapter;
    MetaSet const& m = s.next;
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
        note(s.capture, "begin_upload failed (%llu bytes)", static_cast<unsigned long long>(ud.size));
        s.jobStatus = st;
        s.jobDiag   = kDiagAdapterRejected;
        return CompletionKind::Failed;
    }
    s.target    = t;
    s.hasTarget = true;

    u32 diag = kDiagAssetLoadFailed;
    if (!t.dst && ud.size) {
        note(s.capture, "begin_upload returned no destination memory");
        st   = make_status(Code::Internal);
        diag = kDiagAdapterRejected;
    } else if (s.kind == AssetKind::Texture && t.rowPitchAlign > 1) {
        for (u32 i = 0; i < m.layoutLevels; ++i) {
            if (m.layout[m.layoutLevels + i] % t.rowPitchAlign != 0) {
                note(s.capture, "adapter row pitch alignment %llu differs from copy_constraints (%llu)",
                     static_cast<unsigned long long>(t.rowPitchAlign),
                     static_cast<unsigned long long>(ctx->cc.optimalRowPitchAlign));
                st   = make_status(Code::Unsupported);
                diag = kDiagAdapterRejected;
                break;
            }
        }
    }
    if (st.ok()) {
        Source src;
        st = open_source(ctx, s, src, false);
        if (st.ok()) {
            u8* dst = static_cast<u8*>(t.dst);
            st = s.kind == AssetKind::Mesh ? write_mesh(ctx, s, src, dst) : write_texture(ctx, s, src, dst);
            src.close();
        } else {
            diag = s.jobDiag == kDiagStoreMiss ? kDiagAssetLoadFailed : s.jobDiag; // vanished since meta
        }
    }
    a.commit_upload(a.user, t.token); // always: the adapter owns the ticket; a failure frees the object
    if (st.failed()) {
        s.jobStatus = st;
        s.jobDiag   = diag;
        return CompletionKind::Failed;
    }
    return CompletionKind::Uploaded;
}

void post(Context* ctx, Completion const& c) noexcept {
    std::lock_guard<std::mutex> lock(ctx->compMutex);
    KILN_VERIFY(ctx->compCount < ctx->compCap); // one job per slot: never full
    ctx->comp[(ctx->compHead + ctx->compCount) % ctx->compCap] = c;
    ++ctx->compCount;
}

} // namespace

usize store_path(Context const* ctx, AssetKind kind, StrView path, char* out, usize cap) noexcept {
    return store_file_path(StrView(ctx->storeDir, ctx->storeDirLen), kind, path, out, cap);
}

u64 texture_layout(ktx2::TextureDesc const& d, u64 pitchAlign, u64 offsetAlign, u64* outOffset,
                   u64* outPitch) noexcept {
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

void run_job(void* arg) noexcept {
    Slot& s      = *static_cast<Slot*>(arg);
    Context* ctx = s.ctx;
    s.jobStatus  = kOk;
    s.jobDiag    = 0;
    s.capture.reset();
    CompletionKind const k = s.jobStage == Stage::Meta ? run_meta(ctx, s) : run_upload(ctx, s);
    Completion const c{s.index, s.jobGen, k};
    post(ctx, c); // from here on the pump thread may reuse `s`
    ctx->jobsInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace kiln::rt

namespace kiln {

u64 texture_level_layout(TextureDesc const& t, CopyConstraints const& c, u64* offsets,
                         u64* pitches) noexcept {
    FormatInfo const* fi = format_info(t.format);
    if (!fi) return 0;
    constexpr u64 kMaxAlign = u64(1) << 32;
    u64 const pitchAlign    = std::bit_ceil(clamp<u64>(c.optimalRowPitchAlign, 1, kMaxAlign));
    u64 const offsetAlign   = std::bit_ceil(clamp<u64>(c.optimalOffsetAlign, 1, kMaxAlign));
    auto const extent       = [](u32 v, u32 level) noexcept { return level < 32 ? max(v >> level, 1u) : 1u; };
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
