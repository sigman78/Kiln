// examples/headless/main.cpp — load assets through the runtime with no GPU and log every step:
// requests, IO, cook-on-miss, MetaReady, Ready, Failed. --slow and --latency add artificial
// delays so large files visibly take a while. --watch keeps pumping after the assets
// settle and logs Changed events (hot reload, docs/design/hot-reload.md).
#include <kiln/assets.h>
#include <kiln/log.h>
#include <kiln/null_adapter.h>
#if KILN_HEADLESS_HAS_COOK
#include <kiln/cook/provider.h>
#endif

#include "cli.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace kiln;

namespace {

constexpr double kMiB = 1024.0 * 1024.0;

/// For printf's %llu, whatever u64 is on this platform.
constexpr unsigned long long ull(u64 v) noexcept { return v; }

using Clock = std::chrono::steady_clock;
Clock::time_point g_start;

double now_ms() { return std::chrono::duration<double, std::milli>(Clock::now() - g_start).count(); }

void sleep_ms(double ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}

// --- Sinks. The runtime logs and diagnoses through these; the app logs through the same sink.

void log_to_stdout(void*, LogLevel level, StrView category, StrView message) {
    std::printf("%9.1f ms  %-5s %-9.*s %.*s\n", now_ms(), log_level_name(level), KILN_SV(category),
                KILN_SV(message));
}

void diag_to_stdout(void*, Diagnostic const& d) {
    std::printf("%9.1f ms  %-5s K%04u      %.*s%s%.*s: %.*s\n", now_ms(), severity_name(d.severity), d.code,
                KILN_SV(d.asset), d.where.size ? " @" : "", KILN_SV(d.where), KILN_SV(d.message));
}

// --- Artificial slow IO: forwards to the built-in backend after a delay.

struct SlowIo {
    IoBackend const* inner = nullptr;
    double msPerMiB        = 0;
    double latencyMs       = 0;
    IoBackend backend{};

    IoBackend const* init(IoBackend const* in, double perMiB, double latency) {
        inner     = in;
        msPerMiB  = perMiB;
        latencyMs = latency;
        backend   = IoBackend{.open       = &open,
                              .size       = &size,
                              .read_range = &read_range,
                              .close      = &close,
                              .stat       = in->stat ? &stat : nullptr, // --watch polls through it, undelayed
                              .user       = this};
        return &backend;
    }

    static Status open(void* u, StrView path, IoFile* out) {
        auto* s = static_cast<SlowIo*>(u);
        return s->inner->open(s->inner->user, path, out);
    }
    static Status size(void* u, IoFile f, u64* out) {
        auto* s = static_cast<SlowIo*>(u);
        return s->inner->size(s->inner->user, f, out);
    }
    static Status read_range(void* u, IoFile f, u64 offset, u64 n, void* dst) {
        auto* s         = static_cast<SlowIo*>(u);
        double const ms = s->latencyMs + s->msPerMiB * double(n) / kMiB;
        KILN_DEBUG_LOG("slow-io", "read %llu bytes at %llu (+%.1f ms)", ull(n), ull(offset), ms);
        sleep_ms(ms);
        return s->inner->read_range(s->inner->user, f, offset, n, dst);
    }
    static void close(void* u, IoFile f) {
        auto* s = static_cast<SlowIo*>(u);
        s->inner->close(s->inner->user, f);
    }
    static Status stat(void* u, StrView path, IoStat* out) {
        auto* s = static_cast<SlowIo*>(u);
        return s->inner->stat(s->inner->user, path, out);
    }
};

// --- Artificial slow cooking: wraps the provider that kiln_cook installed.

struct SlowCook {
    CookProvider inner{};
    double msPerMiB  = 0;
    double latencyMs = 0;

    static Status cook(void* u, AssetKind kind, StrView path, Allocator const* alloc, Vec<u8>* out,
                       DiagSink const* diag) {
        auto* s         = static_cast<SlowCook*>(u);
        double const t0 = now_ms();
        Status const st = s->inner.cook(s->inner.user, kind, path, alloc, out, diag);
        double const ms = st.ok() ? s->latencyMs + s->msPerMiB * double(out->size()) / kMiB : 0.0;
        KILN_INFO("slow-cook", "%s %.*s: %s in %.1f ms, %llu bytes (+%.1f ms)",
                  kind == AssetKind::Mesh ? "mesh" : "texture", KILN_SV(path),
                  st.ok() ? "cooked" : code_name(st.code), now_ms() - t0, ull(out->size()), ms);
        sleep_ms(ms);
        return st;
    }
};

// --- The assets this run loads.

constexpr u32 kMaxItems = 64;

struct Item {
    StrView path; ///< store-relative, extension stripped
    AssetKind kind = AssetKind::Mesh;
    MeshHandle mesh;
    TextureHandle texture;
    State last = State::Unloaded;
};

char const* state_name(State s) {
    switch (s) {
    case State::Unloaded: return "Unloaded";
    case State::Pending: return "Pending";
    case State::MetaReady: return "MetaReady";
    case State::Ready: return "Ready";
    case State::Failed: return "Failed";
    case State::Partial: return "Partial";
    }
    return "?";
}

char const* event_name(EventKind k) {
    switch (k) {
    case EventKind::MetaReady: return "MetaReady";
    case EventKind::Ready: return "Ready";
    case EventKind::Changed: return "Changed";
    case EventKind::Failed: return "Failed";
    }
    return "?";
}

char const* kind_name(AssetKind k) { return k == AssetKind::Mesh ? "mesh" : "texture"; }

/// An asset name plus a kind suffix: "props/chair.glb.mesh" -> mesh "props/chair.glb";
/// "ui/font.png.ktx2" -> texture "ui/font.png". False otherwise.
bool parse_item(char const* arg, Item& out) {
    usize const n = std::strlen(arg);
    if (n > 5 && std::strcmp(arg + n - 5, ".mesh") == 0) {
        out.path = StrView(arg, n - 5);
        out.kind = AssetKind::Mesh;
        return true;
    }
    if (n > 5 && std::strcmp(arg + n - 5, ".ktx2") == 0) {
        out.path = StrView(arg, n - 5);
        out.kind = AssetKind::Texture;
        return true;
    }
    return false;
}

Item* find_item(Item* items, u32 count, Event const& e) {
    for (u32 i = 0; i < count; ++i) {
        Item& it = items[i];
        if (it.kind != e.asset) continue;
        u64 const bits = it.kind == AssetKind::Mesh ? it.mesh.bits() : it.texture.bits();
        if (bits == e.handle) return &it;
    }
    return nullptr;
}

/// What a host does with metadata: read parts, LODs and bounds; texture format and size.
void print_meta(Context* ctx, Item const& it) {
    if (it.kind == AssetKind::Mesh) {
        mesh::MeshView const* v = mesh_view(ctx, it.mesh);
        if (!v) return;
        mesh::Bounds const& b = v->model().bounds;
        KILN_INFO("app", "  mesh '%.*s': %u parts, %u lods, %u submeshes, %u materials, %u mounts",
                  KILN_SV(v->name()), u32(v->parts().size()), u32(v->lods().size()),
                  u32(v->submeshes().size()), u32(v->materials().size()), u32(v->mounts().size()));
        KILN_INFO("app", "  bounds: center (%.2f %.2f %.2f) radius %.2f; payload %llu bytes in %u blob(s)%s",
                  double(b.center[0]), double(b.center[1]), double(b.center[2]), double(b.radius),
                  ull(v->decoded_size()), u32(v->blobs().size()), v->payload_raw() ? ", raw" : "");
        for (u32 i = 0; i < v->lods().size(); ++i) {
            mesh::MeshLod const& l = v->lods()[i];
            KILN_INFO("app", "  lod %u: %u vertices, %u indices", i, l.vertexCount, l.indexCount);
        }
    } else {
        TextureInfo const info = texture_info(ctx, it.texture);
        KILN_INFO("app", "  texture: %s %ux%u, %u level(s)%s; gpu_object() serves the %s",
                  format_name(info.desc.format), info.desc.width, info.desc.height, info.desc.levels,
                  info.desc.isCube ? ", cube" : "", info.isPlaceholder ? "placeholder" : "real texture");
    }
}

void print_event(Context* ctx, Item* items, u32 count, Event const& e) {
    Item* it = find_item(items, count, e);
    if (!it) {
        KILN_WARN("app", "event %s for an unknown %s handle", event_name(e.kind), kind_name(e.asset));
        return;
    }
    State const s = it->kind == AssetKind::Mesh ? state(ctx, it->mesh) : state(ctx, it->texture);
    KILN_INFO("app", "event %-9s %.*s  (%s -> %s, v%u)", event_name(e.kind), KILN_SV(it->path),
              state_name(it->last), state_name(s), e.version);
    it->last = s;

    switch (e.kind) {
    case EventKind::MetaReady: print_meta(ctx, *it); break;
    case EventKind::Ready: {
        GpuObject const g =
            it->kind == AssetKind::Mesh ? gpu_object(ctx, it->mesh) : gpu_object(ctx, it->texture);
        if (g.slot == kInvalid)
            KILN_INFO("app", "  gpu object native %llu", ull(g.native));
        else
            KILN_INFO("app", "  gpu object native %llu, bindless slot %u", ull(g.native), g.slot);
        break;
    }
    case EventKind::Failed: KILN_ERROR("app", "  failed: %s", code_name(e.status.code)); break;
    case EventKind::Changed: print_meta(ctx, *it); break; // the new version's metadata
    }
}

constexpr u32 kMaxRoots = 8;

char const* const kLayouts[] = {"catalog", "named", nullptr};

struct Options {
    char const* store  = "cooked";
    char const* layout = "catalog";
    Root roots[kMaxRoots]; ///< --source and --root
    u32 rootCount    = 0;
    double slowMs    = 0;
    double latencyMs = 0;

    u32 timeoutS = 60;
    bool trace   = false;
    bool watch   = false;
    Item items[kMaxItems];
    u32 itemCount = 0;
};

/// `--root [<name>=]<dir>` and `--source <dir>`. A prefix before `=` that is a valid root name
/// names the root; otherwise the argument is the default root. create() rejects a repeated root.
bool add_root(void* user, char const* arg) {
    auto* o = static_cast<Options*>(user);
    if (o->rootCount == kMaxRoots) {
        std::fprintf(stderr, "kiln-headless: too many roots (max %u)\n", kMaxRoots);
        return false;
    }
    char const* const eq = std::strchr(arg, '=');
    bool const named     = eq && !check_root_name(StrView(arg, usize(eq - arg)));
    o->roots[o->rootCount++] =
        named ? Root{StrView(arg, usize(eq - arg)), StrView(eq + 1)} : Root{{}, StrView(arg)};
    return true;
}

bool add_item(void* user, char const* arg) {
    auto* o = static_cast<Options*>(user);
    if (o->itemCount == kMaxItems) {
        std::fprintf(stderr, "kiln-headless: too many assets (max %u)\n", kMaxItems);
        return false;
    }
    if (!parse_item(arg, o->items[o->itemCount])) {
        std::fprintf(stderr, "kiln-headless: '%s' needs a .mesh or .ktx2 extension\n", arg);
        return false;
    }
    ++o->itemCount;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    cli::Option const opts[] = {
        {.name = "--store", .arg = "<dir>", .help = "cooked store root (default: cooked)", .str = &o.store},
        {.name    = "--layout",
         .arg     = "<layout>",
         .help    = "the store's layout: catalog (default) or named",
         .str     = &o.layout,
         .choices = kLayouts},
        {.name = "--source",
         .arg  = "<dir>",
         .help = "the default root; enables cook-on-miss (needs kiln_cook)",
         .each = &add_root,
         .user = &o},
        {.name = "--root",
         .arg  = "[<name>=]<dir>",
         .help = "a source root for cook-on-miss; <name>=<dir> names <name>:<path> (repeatable)",
         .each = &add_root,
         .user = &o},
        {.name = "--slow",
         .arg  = "<ms>",
         .help = "artificial delay per MiB read or cooked (default: 0)",
         .real = &o.slowMs},
        {.name = "--latency",
         .arg  = "<ms>",
         .help = "artificial delay per read call and per cook (default: 0)",
         .real = &o.latencyMs},

        {.name   = "--timeout",
         .arg    = "<s>",
         .help   = "give up after this many seconds (default: 60)",
         .number = &o.timeoutS},
        {.name = "--trace", .help = "also show the runtime's debug log", .flag = &o.trace},
        {.name = "--watch",
         .help = "after settling, pump until --timeout and log hot reloads (re-cooks with --source)",
         .flag = &o.watch},
    };
    cli::Spec const spec{
        .program  = "kiln-headless",
        .synopsis = "[options] <asset>...",
        .options  = {opts, countof(opts)},
        .footer =
            "<asset> is an asset name plus .mesh or .ktx2 for its kind, e.g. mesh/Box.glb.mesh or\n"
            "ui/font.png.ktx2.\n"
            "Exit codes: 0 every asset Ready, 1 one or more Failed or timed out, 2 usage or setup error.",
        .positional = &add_item,
        .user       = &o,
    };
    cli::Result const args = cli::parse(spec, argc, argv);
    if (args.help) return 0;
    if (!args.ok || o.itemCount == 0) {
        cli::usage(spec, stderr);
        return 2;
    }
    g_start = Clock::now();

    // 1. Logging: one process-wide sink; the runtime and this program both write to it.
    set_log_sink(LogSink{&log_to_stdout, nullptr});
    set_log_level(o.trace ? LogLevel::Debug : LogLevel::Info);

    // 2. An adapter. The null adapter keeps uploads in CPU memory and completes them at once.
    //    That makes it self-submitting, so wait() on a group would be allowed as well.
    Adapter adapter{};
    Result<NullAdapter*> na = null_adapter_create({}, &adapter);
    if (na.failed()) {
        KILN_ERROR("app", "null_adapter_create: %s", code_name(na.code()));
        return 2;
    }

    // 3. The context. Everything below runs on this thread, the pump thread.
    SlowIo slowIo;
    ContextDesc desc{
        .diag        = DiagSink{&diag_to_stdout, nullptr},
        .io          = slowIo.init(compat_io_backend(), o.slowMs, o.latencyMs),
        .adapter     = &adapter,
        .storeDir    = StrView(o.store),
        .roots       = Span<Root const>(o.roots, o.rootCount),
        .storeLayout = std::strcmp(o.layout, "named") == 0 ? StoreLayout::Named : StoreLayout::Catalog,
        .hotReload   = {.watchStore = o.watch},
    };
    Result<Context*> c = create(desc);
    if (c.failed()) {
        KILN_ERROR("app", "create: %s", code_name(c.code()));
        null_adapter_destroy(na.value());
        return 2;
    }
    Context* ctx = c.value();
    KILN_INFO("app", "context ready: store '%s', %u source root(s), slow %.0f ms/MiB, latency %.0f ms",
              o.store, o.rootCount, o.slowMs, o.latencyMs);

    // 4. Cook-on-miss (dev builds): assets missing from the store are cooked from the source roots
    //    on a worker and written to the store. The slow wrapper sits in front of the provider.
    SlowCook slowCook;
    bool providerInstalled = false;
#if KILN_HEADLESS_HAS_COOK
    if (o.rootCount) {
        Status const st = cook::install_provider(ctx, cook::ProviderDesc{.watchSources = o.watch});
        if (st.failed()) {
            KILN_ERROR("app", "install_provider: %s", code_name(st.code));
        } else {
            providerInstalled = true;
            if (o.slowMs > 0 || o.latencyMs > 0) {
                slowCook.inner     = cook_provider(ctx);
                slowCook.msPerMiB  = o.slowMs;
                slowCook.latencyMs = o.latencyMs;
                set_cook_provider(ctx, CookProvider{&SlowCook::cook, &slowCook});
            }
        }
    }
#else
    if (o.rootCount) KILN_WARN("app", "built without kiln_cook: --source and --root are ignored");
    (void)slowCook;
#endif

    // 5. Requests. One load group tracks progress over the whole set.
    Group const g = group(ctx);
    for (u32 i = 0; i < o.itemCount; ++i) {
        Item& it = o.items[i];
        if (it.kind == AssetKind::Mesh)
            it.mesh = request_mesh(ctx, it.path, RequestOptions{.group = g});
        else
            it.texture = request_texture(ctx, it.path, RequestOptions{.group = g});
        it.last = it.kind == AssetKind::Mesh ? state(ctx, it.mesh) : state(ctx, it.texture);
        KILN_INFO("app", "request %-7s %.*s  -> %s", kind_name(it.kind), KILN_SV(it.path),
                  state_name(it.last));
    }

    // 6. The pump loop. pump() is the only place where state changes become visible. With
    //    --watch it keeps going after the group settles, until --timeout, so reloads show up.
    u32 pumps            = 0;
    double lastProgress  = 0;
    double const limitMs = double(o.timeoutS) * 1000.0; // wall clock: sleeps are coarse on Windows
    GroupStatus gs       = progress(ctx, g);
    bool timedOut        = false;
    bool watching        = false;
    for (;;) {
        PumpStats const ps = pump(ctx);
        ++pumps;
        Span<Event const> const evs = events(ctx);
        for (Event const& e : evs)
            print_event(ctx, o.items, o.itemCount, e);
        bool const idle = evs.empty() && ps.completed == 0 && ps.uploadsCommitted == 0;
        gs              = progress(ctx, g);
        if (o.watch && !watching && gs.settled()) {
            watching = true;
            KILN_INFO("app", "settled; watching for changes until --timeout (%u s from start)", o.timeoutS);
        }
        // Progress when something completed, and about once a second while waiting.
        if (!idle || (!watching && now_ms() - lastProgress >= 1000.0)) {
            ContextStats const cs = stats(ctx);
            KILN_INFO("app", "ready %u failed %u pending %u, %llu/%llu bytes, io jobs %u, uploads %u",
                      gs.ready, gs.failed, gs.pending, ull(gs.bytesDone), ull(gs.bytesTotal),
                      cs.ioJobsInFlight, cs.uploadsInFlight);
            lastProgress = now_ms();
        }
        if (gs.settled() && !o.watch) break;
        if (now_ms() >= limitMs) {
            timedOut = !gs.settled(); // with --watch, the end of the watch is not a timeout
            break;
        }
        if (idle) sleep_ms(1.0); // rest instead of spinning on an empty pump
    }

    // 7. Summary and teardown. release() is refcounted; destroy() drops whatever is left.
    NullAdapterStats const as = null_adapter_stats(na.value());
    KILN_INFO("app",
              "%s in %.1f ms (%u pumps): %u ready, %u failed, %u pending; adapter holds %llu bytes in %u "
              "object(s)",
              timedOut ? "timed out" : "settled", now_ms(), pumps, gs.ready, gs.failed, gs.pending,
              ull(as.bytesUploaded), as.liveObjects);

    for (u32 i = 0; i < o.itemCount; ++i) {
        Item const& it = o.items[i];
        if (it.kind == AssetKind::Mesh)
            release(ctx, it.mesh);
        else
            release(ctx, it.texture);
    }
    release(ctx, g);
#if KILN_HEADLESS_HAS_COOK
    if (providerInstalled) cook::uninstall_provider(ctx);
#else
    (void)providerInstalled;
#endif
    destroy(ctx);
    null_adapter_destroy(na.value());
    return (gs.failed != 0 || timedOut) ? 1 : 0;
}
