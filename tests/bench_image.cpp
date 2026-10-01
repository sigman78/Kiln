// tests/bench_image.cpp — manual image-kernel benchmark (rollout step 1,
// docs/design/cook-kernels.md). Not a unit test: this is the "before" (and later
// "after") baseline for the step 2 kernel work, so it gets its own executable and
// its own main(), never wired into ctest. Build and run it directly:
//   build/<preset>/tests/kiln_bench_image [--max 2048|4096|8192] [--repeat N] [--threads N]
// --threads N counts the calling thread, like kiln-cook: 1 (the default) passes no job
// system, 0 means one thread per core, n means a pool of n - 1 workers.
//
// Every source image is synthetic LCG noise, generated in memory (no file IO, no
// <random>: a fixed seed keeps runs comparable). The one exception is the
// cook_texture end-to-end row, which needs a real PNG (tests/png_writer.h).
#include "no_crash_dialogs.h"
#include "png_writer.h"

#include "kiln/containers.h"
#include "kiln/cook/cook.h"
#include "kiln/cook/image.h"
#include "kiln/io.h"
#include "kiln/log.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace kiln;
using namespace kiln::cook;
namespace png = kiln::test::png;

namespace {

// ---------------------------------------------------------------------------
// Synthetic images: fixed-seed LCG noise, no <random> (CLAUDE.md: no non-essential
// heavy std headers; this also keeps byte-for-byte reproducible input across runs).
// ---------------------------------------------------------------------------

Image make_image(u32 w, u32 h, u32 channels, u32 seed) {
    Image img;
    img.width          = w;
    img.height         = h;
    img.channels       = channels;
    img.bitsPerChannel = 8;
    img.pixels.init(default_allocator(), Tag::Test);
    usize const n = usize(img.byte_size());
    Span<u8> buf  = img.pixels.append_uninit(n);
    u32 state     = seed;
    for (usize i = 0; i < n; ++i) {
        state       = state * 1664525u + 1013904223u;
        buf.data[i] = u8(state >> 24);
    }
    return img;
}

/// Image has no copy constructor (Vec<u8> copies are explicit): field-by-field clone
/// for stages that need a fresh, unmutated buffer on every repeat.
Image clone_image(Image const& src) {
    Image out;
    out.width          = src.width;
    out.height         = src.height;
    out.channels       = src.channels;
    out.bitsPerChannel = src.bitsPerChannel;
    out.pixels         = src.pixels.clone();
    return out;
}

// ---------------------------------------------------------------------------
// Stage table: {name, run, srcBytes}. Add a new kernel here to bench it too.
// ---------------------------------------------------------------------------

// flip_green / renormalize work in place, so their timing includes a clone of the
// source (a memcpy, small next to the kernel).
void stage_convert(Image const& rgb8, Image const&, Allocator const* alloc, JobBudget const& jobs) {
    Result<Image> r = convert_image(rgb8, 4, 8, alloc, jobs);
    KILN_VERIFY(r.ok());
}
void stage_flip_green(Image const&, Image const& rgba8, Allocator const*, JobBudget const& jobs) {
    Image img = clone_image(rgba8);
    flip_green(img, jobs);
}
void stage_renormalize(Image const&, Image const& rgba8, Allocator const*, JobBudget const& jobs) {
    Image img = clone_image(rgba8);
    renormalize(img, jobs);
}
void stage_downsample_linear(Image const&, Image const& rgba8, Allocator const* alloc,
                             JobBudget const& jobs) {
    Result<Image> r = downsample_2x(rgba8, MipOptions{.srgb = false}, alloc, jobs);
    KILN_VERIFY(r.ok());
}
void stage_downsample_srgb(Image const&, Image const& rgba8, Allocator const* alloc, JobBudget const& jobs) {
    Result<Image> r = downsample_2x(rgba8, MipOptions{.srgb = true}, alloc, jobs);
    KILN_VERIFY(r.ok());
}
void stage_downsample_renorm(Image const&, Image const& rgba8, Allocator const* alloc,
                             JobBudget const& jobs) {
    Result<Image> r = downsample_2x(rgba8, MipOptions{.renormalize = true}, alloc, jobs);
    KILN_VERIFY(r.ok());
}
void stage_build_mip_chain(Image const&, Image const& rgba8, Allocator const* alloc, JobBudget const& jobs) {
    Result<Vec<Image>> r = build_mip_chain(clone_image(rgba8), MipOptions{.srgb = true}, 0, alloc, jobs);
    KILN_VERIFY(r.ok());
}

u64 bytes_of_rgb8(Image const& rgb8, Image const&) noexcept { return rgb8.byte_size(); }
u64 bytes_of_rgba8(Image const&, Image const& rgba8) noexcept { return rgba8.byte_size(); }

struct StageSpec {
    char const* name;
    void (*run)(Image const& rgb8, Image const& rgba8, Allocator const* alloc, JobBudget const& jobs);
    u64 (*srcBytes)(Image const& rgb8, Image const& rgba8) noexcept;
};

constexpr StageSpec kStages[] = {
    {"convert_image RGB8->RGBA8", &stage_convert,           &bytes_of_rgb8 },
    {"flip_green",                &stage_flip_green,        &bytes_of_rgba8},
    {"renormalize",               &stage_renormalize,       &bytes_of_rgba8},
    {"downsample_2x linear",      &stage_downsample_linear, &bytes_of_rgba8},
    {"downsample_2x srgb",        &stage_downsample_srgb,   &bytes_of_rgba8},
    {"downsample_2x renormalize", &stage_downsample_renorm, &bytes_of_rgba8},
    {"build_mip_chain srgb",      &stage_build_mip_chain,   &bytes_of_rgba8},
};

// ---------------------------------------------------------------------------
// Timing and the printed table
// ---------------------------------------------------------------------------

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

double mib_per_sec(u64 bytes, double ms) noexcept {
    return ms > 0.0 ? (double(bytes) / (1024.0 * 1024.0)) / (ms / 1000.0) : 0.0;
}

void print_row(char const* name, char const* size, double ms, double mibs) {
    std::printf("%-28s %-11s %10.3f %10.1f\n", name, size, ms, mibs);
}

void print_header() {
    std::printf("%-28s %-11s %10s %10s\n", "stage", "size", "ms", "MiB/s");
    std::printf("%-28s %-11s %10s %10s\n", "----------------------------", "-----------", "----------",
                "----------");
}

} // namespace

int main(int argc, char** argv) {
    no_crash_dialogs();
    u32 maxSize = 4096;
    u32 repeat  = 3;
    u32 threads = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--max") == 0 && i + 1 < argc) {
            maxSize = u32(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = u32(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = u32(std::atoi(argv[++i]));
        } else {
            std::fprintf(stderr,
                         "usage: kiln_bench_image [--max 2048|4096|8192] [--repeat N] [--threads N]\n");
            return 1;
        }
    }
    if (repeat == 0) repeat = 1;

    Allocator const* alloc = default_allocator();
    JobSystem pool;
    JobSystem const* jobs = nullptr;
    JobBudget budget{nullptr, threads};
    if (threads != 1) {
        Result<JobSystem> created = create_thread_pool({.threads = threads ? threads - 1 : 0});
        KILN_VERIFY(created.ok());
        pool        = *created;
        jobs        = &pool;
        budget.jobs = jobs;
        std::printf("kiln_bench_image: max %u, repeat %u (best of N reported), threads: pool of %u workers "
                    "+ calling thread\n\n",
                    maxSize, repeat, thread_pool_thread_count(pool));
    } else {
        std::printf(
            "kiln_bench_image: max %u, repeat %u (best of N reported), threads: none (single-threaded)"
            "\n\n",
            maxSize, repeat);
    }
    print_header();

    static constexpr u32 kSweepSizes[] = {2048, 4096, 8192};
    for (u32 size : kSweepSizes) {
        if (size > maxSize) continue;
        Image const rgb8  = make_image(size, size, 3, 0x1234'5678u ^ size);
        Image const rgba8 = make_image(size, size, 4, 0x8765'4321u ^ size);

        char sizeStr[16];
        format(sizeStr, sizeof sizeStr, "%ux%u", size, size);
        for (StageSpec const& st : kStages) {
            double best = -1.0;
            for (u32 r = 0; r < repeat; ++r) {
                auto const t0 = std::chrono::steady_clock::now();
                st.run(rgb8, rgba8, alloc, budget);
                double const ms = ms_since(t0);
                if (best < 0.0 || ms < best) best = ms;
            }
            print_row(st.name, sizeStr, best, mib_per_sec(st.srcBytes(rgb8, rgba8), best));
        }
    }

    // cook_texture end to end: a real PNG through the full pipeline, fixed at 2048
    // regardless of --max, plus its CookStats breakdown (our step-1 payoff).
    {
        u32 const size         = 2048;
        Image const rgba8      = make_image(size, size, 4, 0xC0FF'EEu);
        Vec<u8> const pngBytes = png::encode(
            {.width = size, .height = size, .colorType = 6, .depth = 8, .pixels = rgba8.pixels.span()});
        KILN_VERIFY(!pngBytes.empty());

        constexpr TextureCookSettings kSettings = {.colorSpace = ColorSpace::Srgb,
                                                   .usage      = TextureUsage::Color};
        constexpr TargetProfile kTarget         = {};

        double best = -1.0;
        CookStats stats{};
        for (u32 r = 0; r < repeat; ++r) {
            auto const t0 = std::chrono::steady_clock::now();
            Result<CookedTexture> c =
                cook_texture({.bytes = pngBytes.span(), .assetPath = "bench/tex", .sourcePath = "bench.png"},
                             kSettings, kTarget, {.alloc = alloc, .jobs = jobs, .maxThreads = threads});
            double const ms = ms_since(t0);
            KILN_VERIFY(c.ok());
            if (best < 0.0 || ms < best) {
                best  = ms;
                stats = c->stats;
            }
        }
        char sizeStr[16];
        format(sizeStr, sizeof sizeStr, "%ux%u", size, size);
        print_row("cook_texture end-to-end", sizeStr, best, mib_per_sec(u64(pngBytes.size()), best));

        std::printf("\ncook_texture CookStats (best of %u): decode %.3f ms, prepare %.3f ms, mips %.3f ms, "
                    "write %.3f ms (total %.3f ms)\n",
                    repeat, double(stats.decodeUs) / 1000.0, double(stats.prepareUs) / 1000.0,
                    double(stats.mipsUs) / 1000.0, double(stats.writeUs) / 1000.0,
                    double(stats.totalUs) / 1000.0);
    }
    if (jobs) destroy_thread_pool(pool);
    return 0;
}
