// One cook of one source file, with a record of every input it read (unit.h).
#include "unit.h"

#include "kiln/cook/sidecar.h"

#include <cerrno>

#if defined(KILN_OS_WINDOWS)
#include <windows.h> // GetFileAttributesExW; WIN32_LEAN_AND_MEAN/NOMINMAX set by kiln_apply_defaults
#else
#include <sys/stat.h>
#endif

namespace kiln::cook {

namespace {

u32 add_string(CookUnit& u, StrView s) noexcept {
    u32 const off = u32(u.strings.size());
    u.strings.append(Span<char const>(s.data, s.size));
    return off;
}

void add_input(CookUnit& u, InputRole role, StrView name, StrView path, IoStat const& stat,
               Hash128 const& content) noexcept {
    UnitInput in;
    in.role    = role;
    in.nameOff = add_string(u, name);
    in.nameLen = u32(name.size);
    in.pathOff = add_string(u, path);
    in.pathLen = u32(path.size);
    in.stat    = stat;
    in.content = content;
    u.inputs.push_back(in);
}

/// Reads `path` into `out` and records it as an input.
Status read_input(UnitDesc const& d, CookUnit& u, InputRole role, StrView name, StrView path,
                  Allocator const* alloc, Vec<u8>* out) noexcept {
    IoStat st{};
    // Stat before the read, so an edit during the cook reads as a change later.
    if (d.statInputs && stat_file(path, &st).failed()) st = {};
    KILN_TRY(io_read_file(compat_io_backend(), path, alloc, out));
    add_input(u, role, name, path, st, xxh3_128(out->span()));
    return kOk;
}

/// The resolution inputs of one output. The sidecar text, if any, lives in `sidecarBytes`.
struct Layers {
    ResolveDesc desc;
    Vec<u8> sidecarBytes;
    char sidecarPath[1100] = {};
};

/// Fills `l` for `name`. With `readSidecar`, reads and records the source's `.kiln` file when
/// it exists (embedded images have none).
Status prepare_layers(UnitDesc const& d, CookUnit& u, StrView name, SlotHint slot, bool readSidecar,
                      Allocator const* alloc, Layers* l) noexcept {
    l->desc = ResolveDesc{
        .asset     = {name, d.sourcePath, slot},
        .nameRules = d.nameRules,
        .policy    = d.policy,
        .target    = *d.target,
        .session   = d.session,
        .diag      = d.env.diag,
    };
    if (!readSidecar) return kOk;
    StrView const path(l->sidecarPath, format(l->sidecarPath, sizeof l->sidecarPath, "%.*s%.*s",
                                              KILN_SV(d.sourcePath), KILN_SV(kSidecarExt)));
    if (!io_file_exists(path)) {
        add_input(u, InputRole::Sidecar, d.name, path, {}, {});
        u.inputs.back().present = false;
        return kOk;
    }
    l->sidecarBytes.init(alloc, Tag::Cook);
    KILN_TRY(read_input(d, u, InputRole::Sidecar, d.name, path, alloc, &l->sidecarBytes));
    l->desc.sidecar = StrView(reinterpret_cast<char const*>(l->sidecarBytes.data()), l->sidecarBytes.size());
    l->desc.sidecarPath = path;
    return kOk;
}

Hash128 output_key(UnitDesc const& d, CookUnit const& u, AssetKind kind, StrView name,
                   u64 settingsHash) noexcept {
    Vec<BuildInput> inputs(u.inputs.allocator(), Tag::Cook);
    inputs.resize(u.inputs.size());
    unit_build_inputs(u, inputs.data());
    return build_key({.kind         = kind,
                      .name         = name,
                      .targetHash   = hash_target(*d.target),
                      .settingsHash = settingsHash,
                      .inputs       = inputs.span()});
}

UnitOutput& add_output(CookUnit& u, AssetKind kind, StrView name, SlotHint slot) noexcept {
    UnitOutput o;
    o.nameOff = add_string(u, name);
    o.nameLen = u32(name.size);
    o.kind    = kind;
    o.slot    = slot;
    o.bytes.init(u.outputs.allocator(), Tag::Cook);
    u.outputs.push_back(std::move(o));
    return u.outputs.back();
}

/// Cooks one texture from `bytes` into a new output. Records a failure in the output.
Status cook_one_texture(UnitDesc const& d, CookUnit& u, Span<u8 const> bytes, StrView name, SlotHint slot,
                        bool ownSource, Allocator const* alloc) noexcept {
    Layers layers;
    Status st = prepare_layers(d, u, name, slot, ownSource, alloc, &layers);
    Result<TextureCookSettings> rs =
        st.ok() ? resolve_texture_layers(*d.textureDefaults, layers.desc) : Result<TextureCookSettings>(st);
    Result<CookedTexture> r =
        rs.failed() ? Result<CookedTexture>(rs.status())
                    : cook_texture({.bytes = bytes, .assetPath = name, .sourcePath = d.sourcePath}, *rs,
                                   *d.target, d.env);
    // Keys are computed once every input is recorded (the sidecar comes in during prepare_layers).
    UnitOutput& o = add_output(u, AssetKind::Texture, name, slot);
    if (r.failed()) return o.status = r.status();
    o.settingsHash = hash_settings(*rs);
    o.key          = output_key(d, u, AssetKind::Texture, name, o.settingsHash);
    o.bytes        = std::move(r->file);
    o.stats        = r->stats;
    return kOk;
}

struct ResolverCtx {
    UnitDesc const* d = nullptr;
    CookUnit* u       = nullptr;
    StrView baseDir;
};

Status resolve_uri_fn(void* user, StrView uri, Allocator const* alloc, Vec<u8>* out) noexcept {
    auto const* c = static_cast<ResolverCtx const*>(user);
    char path[1200];
    usize const n = format(path, sizeof path, "%.*s/%.*s", KILN_SV(c->baseDir), KILN_SV(uri));
    if (n >= sizeof path - 1) return make_status(Code::InvalidArgument);
    StrView const pathView(path, n);
    if (!io_file_exists(pathView)) return make_status(Code::NotFound);
    return read_input(*c->d, *c->u, InputRole::Buffer, uri, pathView, alloc, out);
}

Status cook_mesh_unit(UnitDesc const& d, CookUnit& u, Span<u8 const> bytes, Allocator const* alloc) noexcept {
    usize const slash = d.sourcePath.rfind('/');
    ResolverCtx rctx{&d, &u, slash == StrView::kNpos ? StrView(".") : d.sourcePath.substr(0, slash)};

    Layers layers;
    KILN_TRY(prepare_layers(d, u, d.name, SlotHint::None, true, alloc, &layers));
    Result<MeshCookSettings> rm = resolve_mesh_layers(*d.meshDefaults, layers.desc);
    if (rm.failed()) return rm.status();

    MeshSource src{};
    src.bytes            = bytes;
    src.assetPath        = d.name;
    src.sourcePath       = d.sourcePath;
    src.resolver         = {&resolve_uri_fn, &rctx};
    Result<CookedMesh> r = cook_mesh(src, *rm, *d.target, d.env);
    if (r.failed()) return r.status();

    UnitOutput& mesh  = add_output(u, AssetKind::Mesh, d.name, SlotHint::None);
    mesh.settingsHash = hash_settings(*rm);
    mesh.key          = output_key(d, u, AssetKind::Mesh, d.name, mesh.settingsHash);
    mesh.bytes        = std::move(r->file);
    mesh.stats        = r->stats;

    for (TextureRef const& t : r->textures)
        if (!t.embedded.empty()) (void)cook_one_texture(d, u, t.embedded, t.assetPath, t.slot, false, alloc);
    return kOk;
}

} // namespace

UnitOutput* CookUnit::find(AssetKind kind, StrView n) noexcept {
    for (UnitOutput& o : outputs)
        if (o.kind == kind && name(o) == n) return &o;
    return nullptr;
}

Status CookUnit::first_failure() const noexcept {
    for (UnitOutput const& o : outputs)
        if (o.status.failed()) return o.status;
    return kOk;
}

void unit_build_inputs(CookUnit const& unit, BuildInput* out) noexcept {
    for (usize i = 0; i < unit.inputs.size(); ++i) {
        UnitInput const& in = unit.inputs[i];
        out[i]              = BuildInput{in.role, unit.str(in.nameOff, in.nameLen), in.content};
    }
}

u64 host_digest(UnitDesc const& d, u32 policyVersion) noexcept {
    Xxh64State h;
    h.update_value(kCookerVersion);
    h.update_value(hash_target(*d.target));
    h.update_value(hash_settings(*d.meshDefaults));
    h.update_value(hash_settings(*d.textureDefaults));
    h.update_value(u32(d.nameRules.size));
    for (NameRule const& r : d.nameRules) {
        h.update_value(u32(r.suffix.size));
        h.update(r.suffix);
        h.update_value(u8(r.usage));
        h.update_value(u8(r.shape));
    }
    h.update_value(u8(d.session.storeMode));
    h.update_value(u8(d.session.fastPreview));
    h.update_value(policyVersion);
    return h.digest();
}

bool recorded_keys_match(UnitDesc const& d, CookUnit const& rec) noexcept {
    Allocator const* alloc = d.env.alloc ? d.env.alloc : default_allocator();
    Vec<u8> sidecar(alloc, Tag::Cook);
    StrView sidecarPath;
    for (UnitInput const& in : rec.inputs) {
        if (in.role != InputRole::Sidecar || !in.present) continue;
        sidecarPath = rec.str(in.pathOff, in.pathLen);
        if (io_read_file(compat_io_backend(), sidecarPath, alloc, &sidecar).failed()) return false;
    }
    Vec<BuildInput> inputs(alloc, Tag::Cook);
    inputs.resize(rec.inputs.size());
    unit_build_inputs(rec, inputs.data());
    u64 const targetHash = hash_target(*d.target);

    for (usize i = 0; i < rec.outputs.size(); ++i) {
        UnitOutput const& o = rec.outputs[i];
        if (o.status.failed()) return false;
        StrView const name = rec.name(o);
        // Only a source of its own has a sidecar (the first output); embedded images have a slot.
        bool const ownSidecar = i == 0;
        ResolveDesc rd{
            .asset     = {name, d.sourcePath, o.slot},
            .nameRules = d.nameRules,
            .policy    = d.policy,
            .target    = *d.target,
            .session   = d.session,
            .diag      = nullptr,
        };
        if (ownSidecar && !sidecarPath.empty()) {
            rd.sidecar     = StrView(reinterpret_cast<char const*>(sidecar.data()), sidecar.size());
            rd.sidecarPath = sidecarPath;
        }
        u64 settingsHash = 0;
        if (o.kind == AssetKind::Mesh) {
            Result<MeshCookSettings> const r = resolve_mesh_layers(*d.meshDefaults, rd);
            if (r.failed()) return false;
            settingsHash = hash_settings(*r);
        } else {
            Result<TextureCookSettings> const r = resolve_texture_layers(*d.textureDefaults, rd);
            if (r.failed()) return false;
            settingsHash = hash_settings(*r);
        }
        Hash128 const key = build_key({.kind         = o.kind,
                                       .name         = name,
                                       .targetHash   = targetHash,
                                       .settingsHash = settingsHash,
                                       .inputs       = inputs.span()});
        if (!(key == o.key)) return false;
    }
    return true;
}

bool recorded_inputs_unchanged(CookUnit const& rec, bool rehash) noexcept {
    Vec<u8> bytes(rec.inputs.allocator(), Tag::Cook);
    for (UnitInput const& in : rec.inputs) {
        StrView const path = rec.str(in.pathOff, in.pathLen);
        IoStat now;
        Status const st = stat_file(path, &now);
        if (!in.present) {
            if (st.code != Code::NotFound) return false;
            continue;
        }
        if (st.failed()) return false;
        if (!rehash) {
            if (now.size != in.stat.size || now.mtimeNs != in.stat.mtimeNs) return false;
            continue;
        }
        bytes.clear();
        if (io_read_file(compat_io_backend(), path, rec.inputs.allocator(), &bytes).failed() ||
            !(xxh3_128(bytes.span()) == in.content))
            return false;
    }
    return true;
}

Status cook_unit(UnitDesc const& d, CookUnit* out) noexcept {
    KILN_VERIFY(d.target && d.meshDefaults && d.textureDefaults);
    Allocator const* alloc = d.env.alloc ? d.env.alloc : default_allocator();
    Vec<u8> bytes(alloc, Tag::Cook);
    KILN_TRY(read_input(d, *out, InputRole::Source, d.name, d.sourcePath, alloc, &bytes));
    if (d.kind == AssetKind::Mesh) return cook_mesh_unit(d, *out, bytes.span(), alloc);
    return cook_one_texture(d, *out, bytes.span(), d.name, SlotHint::None, true, alloc);
}

Status stat_file(StrView path, IoStat* out) noexcept {
    IoBackend const* io = compat_io_backend();
    if (io && io->stat) return io->stat(io->user, path, out);

    char buf[1024];
    if (path.size + 1 > sizeof buf) return make_status(Code::InvalidArgument);
    std::memcpy(buf, path.data, path.size);
    buf[path.size] = '\0';
#if defined(KILN_OS_WINDOWS)
    wchar_t wbuf[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, int(sizeof wbuf / sizeof wbuf[0])) == 0)
        return make_status(Code::InvalidArgument);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(wbuf, GetFileExInfoStandard, &fa)) {
        DWORD const err    = GetLastError();
        bool const missing = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        return make_status(missing ? Code::NotFound : Code::IoError, u16(err & 0xFFFFu));
    }
    out->size = (u64(fa.nFileSizeHigh) << 32) | u64(fa.nFileSizeLow);
    out->mtimeNs =
        ((u64(fa.ftLastWriteTime.dwHighDateTime) << 32) | u64(fa.ftLastWriteTime.dwLowDateTime)) * 100u;
#else
    struct stat st{};
    if (::stat(buf, &st) != 0) {
        int const e = errno;
        return make_status(e == ENOENT ? Code::NotFound : Code::IoError, u16(e & 0xFFFF));
    }
    out->size = u64(st.st_size);
#if defined(KILN_OS_MACOS)
    out->mtimeNs = u64(st.st_mtimespec.tv_sec) * 1000000000ull + u64(st.st_mtimespec.tv_nsec);
#else
    out->mtimeNs = u64(st.st_mtim.tv_sec) * 1000000000ull + u64(st.st_mtim.tv_nsec);
#endif
#endif
    return kOk;
}

} // namespace kiln::cook
