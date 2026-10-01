// One cook of one source file, with a record of every input it read (unit.h).
#include "unit.h"

#include "parallel.h"

#include "kiln/cook/project.h"
#include "kiln/cook/sidecar.h"

#include <cerrno>
#include <mutex>

#if defined(KILN_OS_WINDOWS)
#include <windows.h> // GetFileAttributesExW; WIN32_LEAN_AND_MEAN/NOMINMAX set by kiln_apply_defaults
#else
#include <sys/stat.h>
#endif

namespace kiln::cook {

namespace {

u32 add_string(CookUnit& u, StrView s) {
    u32 const off = u32(u.strings.size());
    u.strings.append(Span<char const>(s.data, s.size));
    return off;
}

void add_input(CookUnit& u, InputRole role, StrView name, StrView path, IoStat const& stat,
               Hash128 const& content) {
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
                  Allocator const* alloc, Vec<u8>* out) {
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
                      Allocator const* alloc, Layers* l) {
    l->desc = ResolveDesc{
        .asset     = {name, d.sourcePath, slot},
        .project   = d.project,
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

Hash128 output_key(UnitDesc const& d, CookUnit const& u, AssetKind kind, StrView name, u64 settingsHash) {
    Vec<BuildInput> inputs(u.inputs.allocator(), Tag::Cook);
    inputs.resize(u.inputs.size());
    unit_build_inputs(u, inputs.data());
    return build_key({.kind         = kind,
                      .name         = name,
                      .targetHash   = hash_target(*d.target),
                      .settingsHash = settingsHash,
                      .inputs       = inputs.span()});
}

UnitOutput& add_output(CookUnit& u, AssetKind kind, StrView name, SlotHint slot) {
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
                        bool ownSource, Allocator const* alloc) {
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

/// One embedded image of a mesh unit, cooked on a worker: the inputs are set before, the outputs
/// read after the parallel_for.
struct ImageJob {
    TextureRef const* ref = nullptr;
    Result<TextureCookSettings> settings{make_status(Code::Unknown)};
    Result<CookedTexture> cooked{make_status(Code::Unknown)};
};

/// The images' diagnostics, one at a time: the host's sink need not be thread-safe.
struct LockedDiag {
    DiagSink const* inner = nullptr;
    std::mutex mutex;
    static void fn(void* user, Diagnostic const& d) {
        auto* l = static_cast<LockedDiag*>(user);
        std::lock_guard<std::mutex> const lock(l->mutex);
        emit(l->inner, d);
    }
};

struct ImageRun {
    UnitDesc const* d = nullptr;
    CookEnv env;
    ImageJob* jobs = nullptr;
};

void cook_images(void* user, u32 begin, u32 end) {
    ImageRun const& r = *static_cast<ImageRun const*>(user);
    for (u32 i = begin; i < end; ++i) {
        ImageJob& j = r.jobs[i];
        if (j.settings.failed()) continue;
        j.cooked = cook_texture(
            {.bytes = j.ref->embedded, .assetPath = j.ref->assetPath, .sourcePath = r.d->sourcePath},
            *j.settings, *r.d->target, r.env);
    }
}

/// Cooks a mesh's embedded images in parallel. Outputs keep the order of `refs`, so the unit is the
/// same as a serial cook's; only the order of diagnostics between images may differ.
void cook_embedded_images(UnitDesc const& d, CookUnit& u, Span<TextureRef const> refs,
                          Allocator const* alloc) {
    Vec<ImageJob> jobs(alloc, Tag::Cook);
    for (TextureRef const& t : refs) {
        if (t.embedded.empty()) continue;
        ImageJob& j = jobs.emplace_back();
        j.ref       = &t;
        Layers layers;
        Status const st               = prepare_layers(d, u, t.assetPath, t.slot, false, alloc, &layers);
        layers.desc.asset.alphaCutoff = t.alphaCutoff;
        j.settings                    = st.ok() ? resolve_texture_layers(*d.textureDefaults, layers.desc)
                                                : Result<TextureCookSettings>(st);
    }
    LockedDiag locked{d.env.diag, {}};
    DiagSink const sink{&LockedDiag::fn, &locked};
    ImageRun run{&d, d.env, jobs.data()};
    run.env.diag = d.env.diag ? &sink : nullptr;
    parallel_for(d.env.jobs, alloc, u32(jobs.size()), 1, &cook_images, &run, d.env.maxThreads);
    for (ImageJob& j : jobs) {
        UnitOutput& o = add_output(u, AssetKind::Texture, j.ref->assetPath, j.ref->slot);
        if (j.settings.failed()) {
            o.status = j.settings.status();
            continue;
        }
        if (j.cooked.failed()) {
            o.status = j.cooked.status();
            continue;
        }
        o.settingsHash = hash_settings(*j.settings);
        o.key          = output_key(d, u, AssetKind::Texture, j.ref->assetPath, o.settingsHash);
        o.bytes        = std::move(j.cooked->file);
        o.stats        = j.cooked->stats;
    }
}

struct ResolverCtx {
    UnitDesc const* d = nullptr;
    CookUnit* u       = nullptr;
    StrView baseDir;
};

Status resolve_uri_fn(void* user, StrView uri, Allocator const* alloc, Vec<u8>* out) {
    auto const* c = static_cast<ResolverCtx const*>(user);
    char path[1200];
    usize const n = format(path, sizeof path, "%.*s/%.*s", KILN_SV(c->baseDir), KILN_SV(uri));
    if (n >= sizeof path - 1) return make_status(Code::InvalidArgument);
    StrView const pathView(path, n);
    if (!io_file_exists(pathView)) return make_status(Code::NotFound);
    return read_input(*c->d, *c->u, InputRole::Buffer, uri, pathView, alloc, out);
}

Status cook_mesh_unit(UnitDesc const& d, CookUnit& u, Span<u8 const> bytes, Allocator const* alloc) {
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

    cook_embedded_images(d, u, r->textures.span(), alloc);
    return kOk;
}

} // namespace

UnitOutput* CookUnit::find(AssetKind kind, StrView n) {
    for (UnitOutput& o : outputs)
        if (o.kind == kind && name(o) == n) return &o;
    return nullptr;
}

Status CookUnit::first_failure() const {
    for (UnitOutput const& o : outputs)
        if (o.status.failed()) return o.status;
    return kOk;
}

void unit_build_inputs(CookUnit const& unit, BuildInput* out) {
    for (usize i = 0; i < unit.inputs.size(); ++i) {
        UnitInput const& in = unit.inputs[i];
        out[i]              = BuildInput{in.role, unit.str(in.nameOff, in.nameLen), in.content};
    }
}

u64 host_digest(UnitDesc const& d, u32 policyVersion) {
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
    h.update_value(u8(d.session.maxQuality));
    h.update_value(policyVersion);
    h.update_value(project_digest(d.project));
    return h.digest();
}

bool recorded_keys_match(UnitDesc const& d, CookUnit const& rec) {
    Allocator const* alloc = d.env.alloc ? d.env.alloc : default_allocator();
    Vec<u8> sidecar(alloc, Tag::Cook);
    char sidecarBuf[1100];
    StrView sidecarPath;
    for (UnitInput const& in : rec.inputs) {
        if (in.role != InputRole::Sidecar || !in.present) continue;
        sidecarPath = StrView(sidecarBuf, input_path(d.sourcePath, in.role, rec.str(in.nameOff, in.nameLen),
                                                     sidecarBuf, sizeof sidecarBuf));
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
            .project   = d.project,
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
        BuildKeyDesc kd{.kind         = o.kind,
                        .name         = name,
                        .targetHash   = targetHash,
                        .settingsHash = settingsHash,
                        .inputs       = inputs.span()};
        if (build_key(kd) == o.key) continue;
        // A quality cap lowers quality for new cooks only: an entry cooked without the cap stays.
        if (o.kind != AssetKind::Texture || d.session.fastPreview ||
            d.session.maxQuality == EncodeQuality::High)
            return false;
        rd.session.maxQuality                  = EncodeQuality::High;
        Result<TextureCookSettings> const full = resolve_texture_layers(*d.textureDefaults, rd);
        if (full.failed()) return false;
        kd.settingsHash = hash_settings(*full);
        if (!(build_key(kd) == o.key)) return false;
    }
    return true;
}

usize input_path(StrView sourcePath, InputRole role, StrView name, char* out, usize cap) {
    if (role == InputRole::Source) return format(out, cap, "%.*s", KILN_SV(sourcePath));
    if (role == InputRole::Sidecar)
        return format(out, cap, "%.*s%.*s", KILN_SV(sourcePath), KILN_SV(kSidecarExt));
    usize const slash = sourcePath.rfind('/');
    StrView const dir = slash == StrView::kNpos ? StrView(".") : sourcePath.substr(0, slash);
    return format(out, cap, "%.*s/%.*s", KILN_SV(dir), KILN_SV(name));
}

InputsCheck check_recorded_inputs(CookUnit& rec, StrView sourcePath, bool rehash) {
    Vec<u8> bytes(rec.inputs.allocator(), Tag::Cook);
    bool touched = false;
    for (UnitInput& in : rec.inputs) {
        char path[1200];
        usize const n = input_path(sourcePath, in.role, rec.str(in.nameOff, in.nameLen), path, sizeof path);
        if (n >= sizeof path - 1) return InputsCheck::Changed;
        StrView const file(path, n);
        IoStat now;
        Status const st = stat_file(file, &now);
        if (!in.present) {
            if (st.code != Code::NotFound) return InputsCheck::Changed;
            continue;
        }
        if (st.failed()) return InputsCheck::Changed;
        bool const same = now.size == in.stat.size && now.mtimeNs == in.stat.mtimeNs;
        if (same && !rehash) continue;
        // A new time alone (a checkout, a save without edits) is no change: the content decides.
        bytes.clear();
        if (io_read_file(compat_io_backend(), file, rec.inputs.allocator(), &bytes).failed() ||
            !(xxh3_128(bytes.span()) == in.content))
            return InputsCheck::Changed;
        if (!same) {
            in.stat = now;
            touched = true;
        }
    }
    return touched ? InputsCheck::Touched : InputsCheck::Unchanged;
}

Status cook_unit(UnitDesc const& d, CookUnit* out) {
    KILN_VERIFY(d.target && d.meshDefaults && d.textureDefaults);
    Allocator const* alloc = d.env.alloc ? d.env.alloc : default_allocator();
    ProfileZone const zone(d.env.profile, "cook.unit", d.name);
    Vec<u8> bytes(alloc, Tag::Cook);
    KILN_TRY(read_input(d, *out, InputRole::Source, d.name, d.sourcePath, alloc, &bytes));
    if (d.kind == AssetKind::Mesh) return cook_mesh_unit(d, *out, bytes.span(), alloc);
    return cook_one_texture(d, *out, bytes.span(), d.name, SlotHint::None, true, alloc);
}

Status stat_file(StrView path, IoStat* out) {
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
