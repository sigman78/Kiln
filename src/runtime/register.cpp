// kiln runtime — in-memory registration (register_mesh / register_texture): copy and
// validate complete cooked bytes, then load them through the normal pipeline with a
// memory source. Pump thread only.
#include "runtime_internal.h"

namespace kiln {
namespace rt {
namespace {

Slot* register_impl(Context* ctx, AssetKind kind, StrView path, Span<u8 const> bytes,
                    RequestOptions const& opt) noexcept {
    if (!ctx) return nullptr;
    char norm[kMaxPathLen];
    usize const len    = normalize_path(path, norm, sizeof norm);
    StrView const name = len == StrView::kNpos ? path : StrView(norm, len);
    if (len != StrView::kNpos && len != 0 && map_for(ctx, kind).contains(fnv1a64(name))) {
        diagf(&ctx->diag, make_status(Code::AlreadyExists), kDiagDuplicateRegister, Severity::Error, name,
              "register", "path is already registered or requested");
        return nullptr;
    }

    Buffer copy;
    copy.allocate(ctx->alloc, bytes.size, Tag::Payload);
    if (bytes.size) std::memcpy(copy.data, bytes.data, bytes.size);
    Status st = kOk;
    if (kind == AssetKind::Mesh) {
        Result<mesh::MeshView> r = mesh::MeshView::open(copy.span(), {}, &ctx->diag, name);
        st                       = r.status();
        if (r.ok() && r->encoded().size != r->header().gpuDataSize) st = make_status(Code::Corrupt);
    } else {
        Result<ktx2::Ktx2View> r = ktx2::Ktx2View::open(copy.span(), &ctx->diag, name);
        st                       = r.status();
        if (r.ok() && !r->has_all_level_data()) st = make_status(Code::Corrupt);
    }
    if (st.failed()) {
        copy.release();
        diagf(&ctx->diag, st, kDiagAssetLoadFailed, Severity::Error, name, "register",
              "registered bytes do not validate (%s)", code_name(st.code));
        return nullptr;
    }
    Slot* s = request_slot(ctx, kind, path, opt, &copy, true);
    copy.release(); // no-op when request_slot took ownership
    return s;
}

} // namespace
} // namespace rt

MeshHandle register_mesh(Context* ctx, StrView path, Span<u8 const> meshFile,
                         RequestOptions const& opt) noexcept {
    rt::Slot* s = rt::register_impl(ctx, AssetKind::Mesh, path, meshFile, opt);
    return s ? MeshHandle::from_bits(rt::handle_bits(*s)) : MeshHandle{};
}

TextureHandle register_texture(Context* ctx, StrView path, Span<u8 const> ktx2File,
                               RequestOptions const& opt) noexcept {
    rt::Slot* s = rt::register_impl(ctx, AssetKind::Texture, path, ktx2File, opt);
    return s ? TextureHandle::from_bits(rt::handle_bits(*s)) : TextureHandle{};
}

} // namespace kiln
