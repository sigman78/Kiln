// src/cook/provider.cpp — cook-on-miss provider. Threading: see docs/design/threading-and-io.md.
// provider_cook() runs on workers, concurrently; a published Provider is never mutated,
// and only install/uninstall touch the registry.
#include "kiln/cook/provider.h"

#include "kiln/cook/cook.h"
#include "kiln/io.h"
#include "kiln/log.h"

#include <cerrno>
#include <mutex>

#if defined(KILN_OS_WINDOWS)
#include <direct.h> // _mkdir
#else
#include <sys/stat.h> // mkdir
#endif

namespace kiln::cook {

namespace {

/// Allocated once by install_provider(); immutable once published.
struct Provider {
    ProviderDesc desc;
    CookSession session;
    MeshCookSettings resolvedMesh; ///< resolve_mesh() is session-static; per-texture
                                   ///< settings depend on SlotHint and are resolved per cook.
    Vec<char> storeDirBuf;         ///< owned copy of store_dir(ctx), NUL-terminated
    StrView storeDir;
    Vec<char> rootsBuf; ///< owned copies of source_roots(ctx), NUL-separated
    Vec<StrView> roots; ///< views into rootsBuf
    Allocator const* alloc = nullptr;

    explicit Provider(Allocator const* a) noexcept
        : storeDirBuf(a, Tag::Cook), rootsBuf(a, Tag::Cook), roots(a, Tag::Cook), alloc(a) {}
};

// Context* -> Provider* registry, under registry_mutex().

std::mutex& registry_mutex() noexcept {
    static std::mutex m;
    return m;
}
HashMap<Context*, Provider*>& registry() noexcept {
    static HashMap<Context*, Provider*> reg(default_allocator(), Tag::Cook);
    return reg;
}

// Path helpers. kiln-cook has its own copy of make_dirs.

bool mkdir_one(char const* path) noexcept {
#if defined(KILN_OS_WINDOWS)
    if (_mkdir(path) == 0) return true;
#else
    if (::mkdir(path, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

/// mkdir -p over the forward-slash path in `buf[0..n)` (NUL-terminated at `n`).
void make_dirs(char* buf, usize n) noexcept {
    for (usize i = 1; i <= n; ++i) {
        if (i == n || buf[i] == '/') {
            char const saved = buf[i];
            buf[i]           = '\0';
            if (!(i == 2 && buf[1] == ':')) // skip a bare "C:" drive letter on Windows
                mkdir_one(buf);
            buf[i] = saved;
        }
    }
}

/// Directory portion of a store-relative asset path, e.g. "meshes/pbr_textures".
StrView dir_part(StrView assetPath) noexcept {
    usize const slash = assetPath.rfind('/');
    return slash == StrView::kNpos ? StrView{} : assetPath.substr(0, slash);
}
/// Leaf portion, e.g. "hull_albedo" out of "meshes/pbr_textures/hull_albedo".
StrView leaf_part(StrView assetPath) noexcept {
    usize const slash = assetPath.rfind('/');
    return slash == StrView::kNpos ? assetPath : assetPath.substr(slash + 1);
}
/// Directory containing a source file path, or "." if it names a bare file.
StrView source_dir(StrView sourcePath) noexcept {
    usize const slash = sourcePath.rfind('/');
    return slash == StrView::kNpos ? StrView(".") : sourcePath.substr(0, slash);
}

/// Writes `bytes` to `<storeDir>/<assetPath>.<ext>` (Named layout), creating
/// every intermediate directory kiln-cook's own default (named) mode would need.
Status write_to_store(StrView storeDir, StrView assetPath, char const* ext, Span<u8 const> bytes,
                      DiagSink const* diag) noexcept {
    StrView const dp = dir_part(assetPath);
    StrView const lp = leaf_part(assetPath);

    char dirBuf[1024];
    usize const dn = dp.empty() ? format(dirBuf, sizeof dirBuf, "%.*s", KILN_SV(storeDir))
                                : format(dirBuf, sizeof dirBuf, "%.*s/%.*s", KILN_SV(storeDir), KILN_SV(dp));
    make_dirs(dirBuf, dn);

    char nameBuf[300];
    usize const nn = format(nameBuf, sizeof nameBuf, "%.*s.%s", KILN_SV(lp), ext);
    return store_write(StrView(dirBuf, dn), StrView(nameBuf, nn), bytes, diag);
}

/// A source file found under one of the provider's roots.
struct FoundSource {
    char path[1024] = {};
    usize len       = 0;
};

/// Tries `<root>/<assetPath>.<ext1>` then `<root>/<assetPath>.<ext2>` for each
/// root in turn (root order outer, extension order inner).
bool find_source(Provider const& p, StrView assetPath, char const* ext1, char const* ext2,
                 FoundSource& out) noexcept {
    for (StrView const& root : p.roots) {
        usize n = format(out.path, sizeof out.path, "%.*s/%.*s.%s", KILN_SV(root), KILN_SV(assetPath), ext1);
        if (io_file_exists(StrView(out.path, n))) {
            out.len = n;
            return true;
        }
        n = format(out.path, sizeof out.path, "%.*s/%.*s.%s", KILN_SV(root), KILN_SV(assetPath), ext2);
        if (io_file_exists(StrView(out.path, n))) {
            out.len = n;
            return true;
        }
    }
    return false;
}

// Mesh URI resolver: external buffers/images relative to the source file.

struct UriResolverCtx {
    char baseDir[900] = {};
};

Status resolve_uri_fn(void* user, StrView uri, Allocator const* alloc, Vec<u8>* out) noexcept {
    auto const* ctx = static_cast<UriResolverCtx const*>(user);
    char path[1200];
    usize const n = format(path, sizeof path, "%s/%.*s", ctx->baseDir, KILN_SV(uri));
    StrView const pathView(path, n);
    if (!io_file_exists(pathView)) return make_status(Code::NotFound);
    return io_read_file(compat_io_backend(), pathView, alloc, out);
}

/// A texture with a source file of its own (no owning mesh involved).
Status cook_texture_own_source(Provider const& p, StrView sourcePath, StrView assetPath,
                               Allocator const* alloc, Vec<u8>* out, DiagSink const* diag) noexcept {
    Vec<u8> bytes(alloc, Tag::Cook);
    KILN_TRY(io_read_file(compat_io_backend(), sourcePath, alloc, &bytes));

    Result<TextureCookSettings> rs =
        resolve_texture(p.desc.texture, SlotHint::None, p.desc.target, p.session, diag, assetPath);
    if (rs.failed()) return rs.status();

    TextureSource src{};
    src.bytes               = bytes.span();
    src.assetPath           = assetPath;
    src.sourcePath          = sourcePath;
    Result<CookedTexture> r = cook_texture(src, *rs, p.desc.target, alloc, diag);
    if (r.failed()) return r.status();

    if (p.desc.storeMode == StoreMode::Disk) {
        Status const st = write_to_store(p.storeDir, assetPath, "ktx2", r->file.span(), diag);
        if (st.failed()) return st;
    }
    *out = std::move(r->file);
    return kOk;
}

/// Cooks the mesh and every texture it references. `out` gets the mesh bytes (Mesh)
/// or the requested texture's bytes (Texture), taken from the in-process result,
/// never re-read from the store (Memory mode has none).
Status cook_mesh_full(Provider const& p, StrView meshAssetPath, StrView sourcePath, AssetKind requestedKind,
                      StrView requestedAssetPath, Allocator const* alloc, Vec<u8>* out,
                      DiagSink const* diag) noexcept {
    Vec<u8> bytes(alloc, Tag::Cook);
    KILN_TRY(io_read_file(compat_io_backend(), sourcePath, alloc, &bytes));

    UriResolverCtx uctx;
    StrView const baseDir = source_dir(sourcePath);
    format(uctx.baseDir, sizeof uctx.baseDir, "%.*s", KILN_SV(baseDir));

    MeshSource src{};
    src.bytes      = bytes.span();
    src.assetPath  = meshAssetPath;
    src.sourcePath = sourcePath;
    src.resolver   = {&resolve_uri_fn, &uctx};

    Result<CookedMesh> r = cook_mesh(src, p.resolvedMesh, p.desc.target, alloc, diag);
    if (r.failed()) return r.status();

    if (p.desc.storeMode == StoreMode::Disk) {
        Status const st = write_to_store(p.storeDir, meshAssetPath, "mesh", r->file.span(), diag);
        if (st.failed()) return st;
    }

    bool requestedFound       = requestedKind == AssetKind::Mesh;
    Status requestedTexStatus = make_status(Code::NotFound);
    Vec<u8> requestedTexBytes(alloc, Tag::Cook);

    for (TextureRef const& t : r->textures) {
        bool const isRequested = requestedKind == AssetKind::Texture && t.assetPath == requestedAssetPath;

        Span<u8 const> texBytes;
        Vec<u8> uriBytes(alloc, Tag::Cook);
        if (!t.embedded.empty()) {
            texBytes = t.embedded;
        } else if (!t.uri.empty()) {
            char path[1200];
            usize const n = format(path, sizeof path, "%.*s/%.*s", KILN_SV(baseDir), KILN_SV(t.uri));
            StrView const pathView(path, n);
            Status const rr = io_read_file(compat_io_backend(), pathView, alloc, &uriBytes);
            if (rr.failed()) {
                diagf(diag, rr, 0, Severity::Error, t.assetPath, "texture", "cannot read external texture %s",
                      path);
                if (isRequested) requestedTexStatus = rr;
                continue;
            }
            texBytes = uriBytes.span();
        } else {
            continue; // no usable source (the importer only emits refs with one)
        }

        Result<TextureCookSettings> rs =
            resolve_texture(p.desc.texture, t.slot, p.desc.target, p.session, diag, t.assetPath);
        if (rs.failed()) {
            if (isRequested) requestedTexStatus = rs.status();
            continue;
        }
        TextureSource tsrc{};
        tsrc.bytes               = texBytes;
        tsrc.assetPath           = t.assetPath;
        tsrc.sourcePath          = sourcePath;
        Result<CookedTexture> tr = cook_texture(tsrc, *rs, p.desc.target, alloc, diag);
        if (tr.failed()) {
            if (isRequested) requestedTexStatus = tr.status();
            continue;
        }

        if (p.desc.storeMode == StoreMode::Disk) {
            Status const st = write_to_store(p.storeDir, t.assetPath, "ktx2", tr->file.span(), diag);
            if (st.failed()) {
                if (isRequested) requestedTexStatus = st;
                continue;
            }
        }
        if (isRequested) {
            requestedTexBytes = std::move(tr->file);
            requestedFound    = true;
        }
    }

    if (requestedKind == AssetKind::Mesh) {
        *out = std::move(r->file);
        return kOk;
    }
    if (!requestedFound) return requestedTexStatus;
    *out = std::move(requestedTexBytes);
    return kOk;
}

Status provider_cook(void* user, AssetKind kind, StrView assetPath, Allocator const* alloc, Vec<u8>* out,
                     DiagSink const* diag) noexcept {
    auto const* p = static_cast<Provider const*>(user);

    if (kind == AssetKind::Texture) {
        FoundSource tex;
        if (find_source(*p, assetPath, "png", "ktx2", tex))
            return cook_texture_own_source(*p, StrView(tex.path, tex.len), assetPath, alloc, out, diag);

        // No source of its own: it must be embedded in its owning mesh
        // ("<mesh assetPath>/<image stem>", cook/cook.h's TextureRef::assetPath).
        usize const slash = assetPath.rfind('/');
        if (slash == StrView::kNpos) return make_status(Code::NotFound);
        StrView const meshAssetPath = assetPath.substr(0, slash);

        FoundSource mesh;
        if (!find_source(*p, meshAssetPath, "glb", "gltf", mesh)) return make_status(Code::NotFound);
        return cook_mesh_full(*p, meshAssetPath, StrView(mesh.path, mesh.len), AssetKind::Texture, assetPath,
                              alloc, out, diag);
    }

    FoundSource mesh;
    if (!find_source(*p, assetPath, "glb", "gltf", mesh)) return make_status(Code::NotFound);
    return cook_mesh_full(*p, assetPath, StrView(mesh.path, mesh.len), AssetKind::Mesh, assetPath, alloc, out,
                          diag);
}

} // namespace

Status install_provider(Context* ctx, ProviderDesc const& desc) noexcept {
    Span<StrView const> const srcRoots = source_roots(ctx);
    if (srcRoots.empty()) return make_status(Code::InvalidArgument);

    ProviderDesc effective = desc;
    if (effective.storeMode == StoreMode::None) {
        KILN_WARN("cook", "ProviderDesc.storeMode == StoreMode::None is not meaningful for a cook "
                          "provider; treating it as StoreMode::Memory");
        effective.storeMode = StoreMode::Memory;
    }

    Allocator const* alloc = allocator(ctx);
    if (!alloc) alloc = default_allocator();

    Provider* p = new_object<Provider>(alloc, Tag::Cook, alloc);
    p->desc     = effective;
    p->session  = CookSession{effective.storeMode, effective.fastPreview};

    StrView const dir = store_dir(ctx);
    p->storeDirBuf.resize(dir.size + 1);
    std::memcpy(p->storeDirBuf.data(), dir.data, dir.size);
    p->storeDirBuf[dir.size] = '\0';
    p->storeDir              = StrView(p->storeDirBuf.data(), dir.size);

    usize total = 0;
    for (StrView const& r : srcRoots)
        total += r.size + 1;
    p->rootsBuf.resize(total);
    p->roots.reserve(srcRoots.size);
    usize offset = 0;
    for (StrView const& r : srcRoots) {
        std::memcpy(p->rootsBuf.data() + offset, r.data, r.size);
        p->rootsBuf[offset + r.size] = '\0';
        p->roots.push_back(StrView(p->rootsBuf.data() + offset, r.size));
        offset += r.size + 1;
    }

    Result<MeshCookSettings> rm = resolve_mesh(effective.mesh, effective.target, p->session);
    if (rm.failed()) {
        delete_object(alloc, p, Tag::Cook);
        return rm.status();
    }
    p->resolvedMesh = rm.value();

    {
        std::lock_guard<std::mutex> const lock(registry_mutex());
        registry().insert(ctx, p);
    }

    CookProvider provider{};
    provider.cook = &provider_cook;
    provider.user = p;
    set_cook_provider(ctx, provider);
    return kOk;
}

void uninstall_provider(Context* ctx) noexcept {
    Provider* p = nullptr;
    {
        std::lock_guard<std::mutex> const lock(registry_mutex());
        if (Provider** found = registry().find(ctx)) {
            p = *found;
            registry().erase(ctx);
        }
    }
    set_cook_provider(ctx, CookProvider{});
    if (p) delete_object(p->alloc, p, Tag::Cook);
}

} // namespace kiln::cook
