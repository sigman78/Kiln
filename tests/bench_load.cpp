// tests/bench_load.cpp — manual benchmark of the runtime's read path: every asset of a cooked store
// through the null adapter. Reports time and pumps to MetaReady and Ready, and where the time goes
// (docs/design/async-read-path.md §9). Not a unit test and not in ctest; usage() lists the options.
#include "no_crash_dialogs.h"

#include "kiln/assets.h"
#include "kiln/containers.h"
#include "kiln/io.h"
#include "kiln/ktx2.h"
#include "kiln/manifest.h"
#include "kiln/null_adapter.h"
#include "kiln/profile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace kiln;

namespace {

// ---------------------------------------------------------------------------
// Profile hooks: time and count per event name (docs/design/cook-tracing.md)
// ---------------------------------------------------------------------------

enum Name : u32 {
    kWaitMeta,
    kWaitPool,
    kMeta,
    kWaitUpload,
    kUpload,
    kOpen,
    kRead,
    kDecode,
    kCopy,
    kGpu,
    kLoad,
    kNameCount
};
constexpr char const* kNames[kNameCount] = {
    "kiln.wait.meta", "kiln.wait.pool", "kiln.meta", "kiln.wait.upload", "kiln.upload", "kiln.open",
    "kiln.read",      "kiln.decode",    "kiln.copy", "kiln.gpu",         "kiln.load"};

u32 name_index(char const* name) {
    for (u32 i = 0; i < kNameCount; ++i)
        if (std::strcmp(name, kNames[i]) == 0) return i;
    return kNameCount;
}

struct Probe {
    std::atomic<u64> ns[kNameCount]    = {};
    std::atomic<u64> count[kNameCount] = {};
    std::atomic<u32> readDepth{0}, readDepthMax{0};
    std::atomic<u32> uploads{0}, directUploads{0};
};

/// The open zones of one thread. Zones nest per thread, so a stack of begin times is enough.
struct ZoneStack {
    static constexpr u32 kMaxDepth = 16;
    u64 t0[kMaxDepth]              = {};
    u32 depth                      = 0;
    bool cpuWork                   = false; ///< the running upload job decoded or copied
};
thread_local ZoneStack tZones;

void zone_begin(void* user, char const* name, StrView) {
    Probe& p     = *static_cast<Probe*>(user);
    ZoneStack& z = tZones;
    if (z.depth < ZoneStack::kMaxDepth) z.t0[z.depth] = profile_now_ns();
    ++z.depth;
    u32 const i = name_index(name);
    if (i == kUpload) z.cpuWork = false;
    if (i == kDecode || i == kCopy) z.cpuWork = true;
    if (i == kRead) {
        u32 const d = p.readDepth.fetch_add(1) + 1;
        u32 m       = p.readDepthMax.load();
        while (d > m && !p.readDepthMax.compare_exchange_weak(m, d)) {
        }
    }
}

void zone_end(void* user, char const* name, StrView) {
    Probe& p     = *static_cast<Probe*>(user);
    ZoneStack& z = tZones;
    if (z.depth == 0) return;
    --z.depth;
    u32 const i = name_index(name);
    if (i == kNameCount) return;
    if (z.depth < ZoneStack::kMaxDepth) {
        p.ns[i].fetch_add(profile_now_ns() - z.t0[z.depth]);
        p.count[i].fetch_add(1);
    }
    if (i == kRead) p.readDepth.fetch_sub(1);
    if (i == kUpload) {
        p.uploads.fetch_add(1);
        if (!z.cpuWork) p.directUploads.fetch_add(1);
    }
}

void interval(void* user, char const* name, StrView, u64 beginNs, u64 endNs) {
    Probe& p    = *static_cast<Probe*>(user);
    u32 const i = name_index(name);
    if (i == kNameCount) return;
    p.ns[i].fetch_add(endNs - beginNs);
    p.count[i].fetch_add(1);
}

// ---------------------------------------------------------------------------
// The corpus: the entries of one profile of the store's manifest
// ---------------------------------------------------------------------------

struct Options {
    char const* store   = nullptr;
    char const* profile = "compat";
    u32 repeat          = 3;
    u32 hz              = 60;
    u32 threads         = 0;
    u32 ioJobs          = 0;
    u32 highEvery       = 0;
    u32 lateHigh        = 0;
    u32 arrayLayers     = 0;
    u32 uploadMiB       = 64;
    u32 pitch           = 1;
    u32 timeoutSec      = 120;
    bool probe          = true;
};

struct Asset {
    StrView name; ///< points into the manifest bytes
    AssetKind kind     = AssetKind::Mesh;
    u64 bytes          = 0;
    TextureShape shape = TextureShape::Tex2D;
    ktx2::TextureDesc desc; ///< textures, when probed
    bool probed = false;
    bool high   = false;
    bool late   = false; ///< requested with high priority once a quarter of the others have settled
};

/// Reads the texture's KTX2 metadata for its shape: a request must name the shape (K5017).
void probe_texture(StrView store, Hash128 const& key, Asset& a) {
    char path[1200];
    if (artifact_file_path(store, key, path, sizeof path) + 1 >= sizeof path) return;
    IoBackend const* io = compat_io_backend();
    IoFile f;
    if (io->open(io->user, StrView(path), &f).failed()) return;
    ktx2::Header h{};
    u64 size = 0;
    if (io->size(io->user, f, &size).ok() && size >= sizeof h &&
        io->read_range(io->user, f, 0, sizeof h, &h).ok()) {
        u64 const msize = ktx2::Ktx2View::metadata_size(h);
        if (msize >= sizeof h && msize <= size && msize <= (u64(1) << 20)) {
            Vec<u8> prefix(default_allocator(), Tag::Test);
            prefix.resize(usize(msize));
            if (io->read_range(io->user, f, 0, msize, prefix.data()).ok()) {
                Result<ktx2::Ktx2View> v = ktx2::Ktx2View::open(prefix.span());
                if (v.ok()) {
                    a.desc   = v->desc();
                    a.probed = true;
                    a.shape  = a.desc.isCube    ? TextureShape::Cube
                               : a.desc.isArray ? TextureShape::Array
                                                : TextureShape::Tex2D;
                }
            }
        }
    }
    io->close(io->user, f);
}

bool same_layer_class(Asset const& a, Asset const& b) {
    return a.desc.format == b.desc.format && a.desc.width == b.desc.width && a.desc.height == b.desc.height &&
           a.desc.levels == b.desc.levels;
}

bool is_layer(Asset const& a) {
    return a.kind == AssetKind::Texture && a.probed && a.shape == TextureShape::Tex2D && a.desc.depth <= 1;
}

/// The names of an array of `layers` layers: the largest set of 2D textures with one format, size and
/// level count, repeated in turn. Empty when the store has no such texture.
void pick_array_layers(Span<Asset const> assets, u32 layers, Vec<StrView>& out) {
    usize best = assets.size, bestCount = 0;
    for (usize i = 0; i < assets.size; ++i) {
        if (!is_layer(assets[i])) continue;
        usize n = 0;
        for (usize j = 0; j < assets.size; ++j)
            n += is_layer(assets[j]) && same_layer_class(assets[i], assets[j]);
        if (n > bestCount) best = i, bestCount = n;
    }
    if (bestCount == 0) return;
    while (out.size() < layers)
        for (usize j = 0; j < assets.size && out.size() < layers; ++j)
            if (is_layer(assets[j]) && same_layer_class(assets[best], assets[j]))
                out.push_back(assets[j].name);
}

// ---------------------------------------------------------------------------
// One run: a new context, every request at once, pump at a fixed rate until all settle
// ---------------------------------------------------------------------------

struct Timing {
    u64 startNs = 0, metaNs = 0, readyNs = 0; ///< the request, from the run's start; then from the request
    u32 startPump = 0, metaPump = 0, readyPump = 0;
    bool done = false, failed = false;
};

struct Run {
    Vec<Timing> t{default_allocator(), Tag::Test};
    bool valid      = false;
    double wallMs   = 0;
    u64 uploadBytes = 0;
    u32 failed = 0, pumps = 0, workers = 0, eventsDropped = 0;
    u64 ns[kNameCount]    = {};
    u64 count[kNameCount] = {};
    u32 readDepthMax = 0, uploads = 0, directUploads = 0;
};

double ms(u64 ns) { return double(ns) / 1e6; }
double mib(u64 bytes) { return double(bytes) / (1024.0 * 1024.0); }

/// Loads `assets`, or the one array of `arrayLayers` when that is not empty.
bool run_once(Options const& o, Span<Asset const> assets, Span<StrView const> arrayLayers, Run& out) {
    usize const requests = arrayLayers.empty() ? assets.size : 1;
    Probe probe;
    Adapter adapter;
    Result<NullAdapter*> na = null_adapter_create(
        {.bindlessSlots = 0, .rowPitchAlign = o.pitch, .maxObjects = u32(requests) + 64}, &adapter);
    if (na.failed()) return false;

    ContextDesc cd;
    cd.adapter               = &adapter;
    cd.storeDir              = StrView(o.store);
    cd.profile               = StrView(o.profile);
    cd.allowUnsampledFormats = true;
    cd.profiler  = {.zone_begin = &zone_begin, .zone_end = &zone_end, .interval = &interval, .user = &probe};
    cd.maxAssets = u32(requests) + 64;
    cd.maxEvents = u32(requests) * 2 + 64;
    cd.workerThreads         = o.threads;
    cd.maxIoJobs             = o.ioJobs;
    Result<Context*> created = create(cd);
    if (created.failed()) {
        std::fprintf(stderr, "create() failed: %s\n", code_name(created.status().code));
        null_adapter_destroy(*na);
        return false;
    }
    Context* ctx = *created;

    out       = Run{};
    out.valid = true;
    out.t.resize(requests);
    out.workers = thread_pool_thread_count(*jobs(ctx));
    Vec<u64> handles(default_allocator(), Tag::Test);
    handles.resize(requests, u64(0));
    Vec<u32> bySlot(default_allocator(), Tag::Test); // registry slot -> request
    bySlot.resize(usize(cd.maxAssets), kInvalid);

    usize remaining    = requests;
    u64 const t0       = profile_now_ns();
    auto const request = [&](usize i) {
        out.t[i].startNs   = profile_now_ns() - t0;
        out.t[i].startPump = out.pumps;
        if (!arrayLayers.empty()) {
            handles[i] = request_texture_array(ctx, {.name = "bench/array", .layers = arrayLayers}).bits();
        } else {
            Asset const& a = assets[i];
            RequestOptions const ro{.priority     = a.high || a.late ? Priority::High : Priority::Normal,
                                    .textureShape = a.shape};
            handles[i] = a.kind == AssetKind::Mesh ? request_mesh(ctx, a.name, ro).bits()
                                                   : request_texture(ctx, a.name, ro).bits();
        }
        if (handles[i] == 0) {
            out.t[i].done = out.t[i].failed = true;
            ++out.failed;
            --remaining;
        } else if (u32(handles[i]) < bySlot.size()) {
            bySlot[u32(handles[i])] = u32(i);
        }
    };
    usize lateCount = 0;
    for (usize i = 0; i < requests; ++i) {
        if (arrayLayers.empty() && assets[i].late)
            ++lateCount;
        else
            request(i);
    }
    usize const lateAfter = (requests - lateCount) / 4; // settled requests before the late ones start

    using Clock = std::chrono::steady_clock;
    Clock::duration const period =
        std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(1'000'000'000ll / o.hz));
    Clock::time_point next = Clock::now();
    while (remaining > 0) {
        PumpStats const ps = pump(ctx, {.uploadBytes = u64(o.uploadMiB) << 20});
        ++out.pumps;
        out.eventsDropped += ps.eventsDropped;
        u64 const now = profile_now_ns() - t0;
        for (Event const& e : events(ctx)) {
            u32 const slot = u32(e.handle);
            if (slot >= bySlot.size() || bySlot[slot] == kInvalid) continue;
            Timing& t = out.t[bySlot[slot]];
            if (e.kind == EventKind::MetaReady) {
                t.metaNs   = now - t.startNs;
                t.metaPump = out.pumps - t.startPump;
            } else if (!t.done) {
                t.readyNs   = now - t.startNs;
                t.readyPump = out.pumps - t.startPump;
                t.done      = true;
                t.failed    = e.kind == EventKind::Failed;
                --remaining;
                if (t.failed && ++out.failed <= 5)
                    std::fprintf(
                        stderr, "  failed (%s): %.*s\n", code_name(e.status.code),
                        KILN_SV(arrayLayers.empty() ? assets[bySlot[slot]].name : StrView("bench/array")));
            }
        }
        if (lateCount && requests - remaining >= lateAfter) {
            for (usize i = 0; i < requests; ++i)
                if (assets[i].late) request(i);
            lateCount = 0;
        }
        if (now > u64(o.timeoutSec) * 1'000'000'000ull) {
            std::fprintf(stderr, "  timeout: %zu requests not settled after %u s\n", remaining, o.timeoutSec);
            out.valid = false;
            break;
        }
        // Spin to the next frame: a sleep is too coarse on Windows (15.6 ms timer).
        next += period;
        if (Clock::time_point const t = Clock::now(); t > next) next = t;
        while (remaining > 0 && Clock::now() < next)
            std::this_thread::yield();
    }
    out.wallMs      = ms(profile_now_ns() - t0);
    out.uploadBytes = null_adapter_stats(*na).bytesUploaded;

    for (usize i = 0; i < requests; ++i) {
        if (handles[i] == 0) continue;
        if (arrayLayers.empty() && assets[i].kind == AssetKind::Mesh)
            release(ctx, MeshHandle::from_bits(handles[i]));
        else
            release(ctx, TextureHandle::from_bits(handles[i]));
    }
    destroy(ctx); // waits for the jobs, so the probe is complete after it
    null_adapter_destroy(*na);
    for (u32 i = 0; i < kNameCount; ++i) {
        out.ns[i]    = probe.ns[i].load();
        out.count[i] = probe.count[i].load();
    }
    out.readDepthMax  = probe.readDepthMax.load();
    out.uploads       = probe.uploads.load();
    out.directUploads = probe.directUploads.load();
    return true;
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

double percentile(Vec<double> const& sorted, u32 pct) {
    if (sorted.empty()) return 0;
    return sorted[min(sorted.size() - 1, sorted.size() * pct / 100)];
}

/// One row: the requests `pick` selects. Times are from the first request, in ms.
void print_class(char const* label, Run const& r, Span<Asset const> assets, bool (*pick)(Asset const&, u64),
                 u64 arg) {
    Vec<double> meta(default_allocator(), Tag::Test), ready(default_allocator(), Tag::Test);
    u64 bytes = 0, pumpSum = 0;
    u32 pumpMax = 0, n = 0;
    for (usize i = 0; i < assets.size; ++i) {
        if (!pick(assets[i], arg)) continue;
        ++n;
        bytes += assets[i].bytes;
        Timing const& t = r.t[i];
        if (!t.done || t.failed) continue;
        meta.push_back(ms(t.metaNs));
        ready.push_back(ms(t.readyNs));
        pumpSum += t.readyPump;
        pumpMax = max(pumpMax, t.readyPump);
    }
    if (n == 0) return;
    std::sort(meta.begin(), meta.end());
    std::sort(ready.begin(), ready.end());
    std::printf("%-12s %6u %9.1f   %8.1f %8.1f %8.1f   %8.1f %8.1f %8.1f   %6.1f %5u\n", label, n, mib(bytes),
                percentile(meta, 50), percentile(meta, 95), percentile(meta, 100), percentile(ready, 50),
                percentile(ready, 95), percentile(ready, 100),
                ready.empty() ? 0.0 : double(pumpSum) / double(ready.size()), pumpMax);
}

bool pick_size(Asset const& a, u64 klass) {
    constexpr u64 kLimits[] = {0, u64(64) << 10, u64(1) << 20, u64(16) << 20, ~u64(0)};
    return a.bytes >= kLimits[klass] && a.bytes < kLimits[klass + 1];
}
bool pick_high(Asset const& a, u64 high) { return a.high == (high != 0); }
bool pick_late(Asset const& a, u64) { return a.late; }
bool pick_all(Asset const&, u64) { return true; }

void print_stages(Run const& r) {
    std::printf("\n%-18s %8s %12s %10s\n", "stage", "count", "total ms", "mean ms");
    for (u32 i = 0; i < kNameCount; ++i) {
        if (r.count[i] == 0) continue;
        std::printf("%-18s %8llu %12.1f %10.3f\n", kNames[i], static_cast<unsigned long long>(r.count[i]),
                    ms(r.ns[i]), ms(r.ns[i]) / double(r.count[i]));
    }
    double const jobMs = ms(r.ns[kMeta] + r.ns[kUpload]);
    if (jobMs <= 0 || r.wallMs <= 0 || r.workers == 0) return;
    std::printf("\nworkers busy: %.0f%% of %u workers over the run\n",
                100.0 * jobMs / (r.wallMs * double(r.workers)), r.workers);
    std::printf("job time: %.0f%% in reads, %.0f%% in opens, %.0f%% in decodes, %.0f%% in copies\n",
                100.0 * ms(r.ns[kRead]) / jobMs, 100.0 * ms(r.ns[kOpen]) / jobMs,
                100.0 * ms(r.ns[kDecode]) / jobMs, 100.0 * ms(r.ns[kCopy]) / jobMs);
    std::printf("reads in flight: at most %u; uploads with no decode or copy (direct reads): %u of %u\n",
                r.readDepthMax, r.directUploads, r.uploads);
}

void print_run_line(u32 index, Run const& r, u64 storeBytes) {
    std::printf("%4u %10.1f %12.1f %13.1f %7u %7u\n", index, r.wallMs, mib(storeBytes) / (r.wallMs / 1000.0),
                mib(r.uploadBytes) / (r.wallMs / 1000.0), r.pumps, r.failed);
    if (r.eventsDropped)
        std::printf("     %u events dropped: the per-asset times are incomplete\n", r.eventsDropped);
}

/// Runs the scenario `repeat` times and keeps the fastest run in `best`.
bool run_scenario(Options const& o, Span<Asset const> assets, Span<StrView const> arrayLayers, u64 storeBytes,
                  Run& best) {
    std::printf("%4s %10s %12s %13s %7s %7s\n", "run", "wall ms", "store MiB/s", "upload MiB/s", "pumps",
                "failed");
    for (u32 i = 0; i < o.repeat; ++i) {
        Run r;
        if (!run_once(o, assets, arrayLayers, r)) return false;
        print_run_line(i + 1, r, storeBytes);
        if (r.valid && (!best.valid || r.wallMs < best.wallMs)) best = std::move(r);
    }
    return best.valid;
}

int usage() {
    std::fprintf(stderr,
                 "usage: kiln_bench_load --store DIR [options]\n"
                 "  --profile NAME   the target profile to load (default compat)\n"
                 "  --repeat N       runs, each with a new context (default 3); the fastest is detailed\n"
                 "  --hz N           pumps per second, 1 to 1000 (default 60)\n"
                 "  --threads N      worker threads (default 0: automatic)\n"
                 "  --io-jobs N      ContextDesc::maxIoJobs (default 0: 16 per worker)\n"
                 "  --upload-mib N   PumpOptions::uploadBytes in MiB (default 64)\n"
                 "  --pitch N        the adapter's row pitch alignment (default 1: no row repacking)\n"
                 "  --high-every N   every N-th request has high priority (default 0: none)\n"
                 "  --late-high N    hold N requests back and make them with high priority once a quarter\n"
                 "                   of the others have settled; their times count from their request\n"
                 "  --array N        also load one texture array of N layers taken from the store\n"
                 "  --no-probe       do not read texture headers first: request every texture as 2D\n"
                 "                   (cubes and arrays then fail). For cold-cache runs.\n"
                 "  --timeout N      seconds before a run is given up (default 120)\n");
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    no_crash_dialogs();
    Options o;
    for (int i = 1; i < argc; ++i) {
        char const* const a   = argv[i];
        char const* const val = i + 1 < argc ? argv[i + 1] : nullptr;
        auto const number     = [&](u32& out) {
            if (val) out = u32(std::atoi(val));
            ++i;
        };
        if (std::strcmp(a, "--no-probe") == 0) {
            o.probe = false;
        } else if (!val) {
            return usage();
        } else if (std::strcmp(a, "--store") == 0) {
            o.store = val, ++i;
        } else if (std::strcmp(a, "--profile") == 0) {
            o.profile = val, ++i;
        } else if (std::strcmp(a, "--repeat") == 0) {
            number(o.repeat);
        } else if (std::strcmp(a, "--hz") == 0) {
            number(o.hz);
        } else if (std::strcmp(a, "--threads") == 0) {
            number(o.threads);
        } else if (std::strcmp(a, "--io-jobs") == 0) {
            number(o.ioJobs);
        } else if (std::strcmp(a, "--upload-mib") == 0) {
            number(o.uploadMiB);
        } else if (std::strcmp(a, "--pitch") == 0) {
            number(o.pitch);
        } else if (std::strcmp(a, "--high-every") == 0) {
            number(o.highEvery);
        } else if (std::strcmp(a, "--late-high") == 0) {
            number(o.lateHigh);
        } else if (std::strcmp(a, "--array") == 0) {
            number(o.arrayLayers);
        } else if (std::strcmp(a, "--timeout") == 0) {
            number(o.timeoutSec);
        } else {
            return usage();
        }
    }
    if (!o.store) return usage();
    o.repeat    = max(o.repeat, 1u);
    o.hz        = clamp(o.hz, 1u, 1000u);
    o.uploadMiB = max(o.uploadMiB, 1u);
    o.pitch     = max(o.pitch, 1u);
    if (o.arrayLayers > kMaxTextureArrayLayers || (o.arrayLayers && !o.probe)) {
        std::fprintf(stderr, "--array takes at most %u layers and needs the probe\n", kMaxTextureArrayLayers);
        return 1;
    }

    char path[1200];
    Vec<u8> manifest(default_allocator(), Tag::Test);
    if (manifest_file_path(StrView(o.store), path, sizeof path) + 1 >= sizeof path ||
        io_read_file(compat_io_backend(), StrView(path), default_allocator(), &manifest).failed()) {
        std::fprintf(stderr, "cannot read the manifest of '%s'\n", o.store);
        return 1;
    }
    Result<ManifestView> view = ManifestView::open(manifest.span());
    ManifestProfile profile;
    if (view.failed() || !view->find_profile(StrView(o.profile), &profile)) {
        std::fprintf(stderr, "the manifest is damaged or has no profile '%s'\n", o.profile);
        return 1;
    }

    Vec<Asset> assets(default_allocator(), Tag::Test);
    u64 storeBytes = 0;
    u32 meshes = 0, lateCount = 0;
    for (u64 i = 0; i < profile.size(); ++i) {
        ManifestEntry const e = profile.entry(i);
        Asset a;
        a.name  = e.name;
        a.kind  = e.kind;
        a.bytes = e.bytes;
        a.high  = o.highEvery && (i + 1) % o.highEvery == 0;
        a.late  = o.lateHigh && i % max<u64>(profile.size() / o.lateHigh, 1) == 0 && lateCount++ < o.lateHigh;
        if (o.probe && e.kind == AssetKind::Texture) probe_texture(StrView(o.store), e.key, a);
        storeBytes += e.bytes;
        meshes += e.kind == AssetKind::Mesh;
        assets.push_back(a);
    }
    if (assets.empty()) {
        std::fprintf(stderr, "profile '%s' has no entries\n", o.profile);
        return 1;
    }
    std::printf("kiln_bench_load: %s, profile %s: %zu assets (%u meshes, %zu textures), %.1f MiB\n", o.store,
                o.profile, assets.size(), meshes, assets.size() - meshes, mib(storeBytes));
    std::printf("pump %u Hz, upload budget %u MiB per pump, row pitch %u, %s\n\n", o.hz, o.uploadMiB, o.pitch,
                o.probe ? "texture headers probed (they are in the file cache now)" : "no probe");

    Run best;
    if (!run_scenario(o, assets.span(), {}, storeBytes, best)) return 1;
    std::printf("\nfastest run, ms from each asset's request: p50 / p95 / max\n");
    std::printf("%-12s %6s %9s   %26s   %26s   %12s\n", "artifact", "count", "MiB", "to MetaReady",
                "to Ready", "pumps avg/max");
    static constexpr char const* kClasses[] = {"< 64 KiB", "< 1 MiB", "< 16 MiB", ">= 16 MiB"};
    for (u64 k = 0; k < countof(kClasses); ++k)
        print_class(kClasses[k], best, assets.span(), &pick_size, k);
    if (o.highEvery) {
        print_class("high", best, assets.span(), &pick_high, 1);
        print_class("normal", best, assets.span(), &pick_high, 0);
    }
    if (o.lateHigh) print_class("late high", best, assets.span(), &pick_late, 0);
    print_class("all", best, assets.span(), &pick_all, 0);
    print_stages(best);

    if (o.arrayLayers) {
        Vec<StrView> layers(default_allocator(), Tag::Test);
        pick_array_layers(assets.span(), o.arrayLayers, layers);
        if (layers.empty()) {
            std::printf("\narray: the store has no 2D texture to use as a layer\n");
        } else {
            std::printf("\narray of %zu layers like %.*s\n", layers.size(), KILN_SV(layers[0]));
            Run array;
            if (!run_scenario(o, {}, layers.span(), 0, array)) return 1;
            if (!array.t[0].failed)
                std::printf("to MetaReady %.1f ms (pump %u), to Ready %.1f ms (pump %u)\n",
                            ms(array.t[0].metaNs), array.t[0].metaPump, ms(array.t[0].readyNs),
                            array.t[0].readyPump);
            print_stages(array);
        }
    }

    std::printf("\npeak bytes over all runs: scratch (Tag::Io) %.1f MiB, payload and staging (Tag::Payload) "
                "%.1f MiB\n",
                mib(default_alloc_stats(Tag::Io).bytesPeak),
                mib(default_alloc_stats(Tag::Payload).bytesPeak));
    return 0;
}
