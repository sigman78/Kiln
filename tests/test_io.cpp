// tests/test_io.cpp — IO layer (kiln/io.h): built-in thread pool and compat IO backend.
// Suite names start with "Io", so `kiln_tests Io` runs the whole file.
#include "kiln_test.h"

#include "kiln/io.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(KILN_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace kiln;

namespace {

void increment_job(void* arg) {
    static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
}

void slow_increment_job(void* arg) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
}

void submit_n(JobSystem const& jobs, void (*fn)(void*), void* arg, int n) {
    for (int i = 0; i < n; ++i)
        jobs.submit(jobs.user, fn, arg);
}

} // namespace

KILN_TEST(IoThreadPool, RunsAllJobsThenWaitIdleSettles) {
    Result<JobSystem> r = create_thread_pool(ThreadPoolDesc{});
    KILN_REQUIRE(r.ok());
    JobSystem jobs = r.value();

    std::atomic<int> counter{0};
    submit_n(jobs, &increment_job, &counter, 1000);
    jobs.wait_idle(jobs.user);

    KILN_CHECK_EQ(counter.load(), 1000);
    destroy_thread_pool(jobs);
}

KILN_TEST(IoThreadPool, SubmitFromMultipleThreads) {
    Result<JobSystem> r = create_thread_pool(ThreadPoolDesc{.threads = 4});
    KILN_REQUIRE(r.ok());
    JobSystem jobs = r.value();

    std::atomic<int> counter{0};
    constexpr int kThreads   = 4;
    constexpr int kPerThread = 250;
    std::thread submitters[kThreads];
    for (auto& t : submitters)
        t = std::thread(&submit_n, std::cref(jobs), &increment_job, &counter, kPerThread);
    for (auto& t : submitters)
        t.join();

    jobs.wait_idle(jobs.user);
    KILN_CHECK_EQ(counter.load(), kThreads * kPerThread);
    destroy_thread_pool(jobs);
}

KILN_TEST(IoThreadPool, QueueFullBackpressureStillCompletes) {
    // Capacity 4, one slow worker: submit() blocks until a slot frees. No lost work, no deadlock.
    Result<JobSystem> r = create_thread_pool(ThreadPoolDesc{.threads = 1, .queueCapacity = 4});
    KILN_REQUIRE(r.ok());
    JobSystem jobs = r.value();

    std::atomic<int> counter{0};
    constexpr int kJobs = 12;
    submit_n(jobs, &slow_increment_job, &counter, kJobs);
    jobs.wait_idle(jobs.user);

    KILN_CHECK_EQ(counter.load(), kJobs);
    destroy_thread_pool(jobs);
}

KILN_TEST(IoThreadPool, ThreadCountRespectsExplicitRequest) {
    Result<JobSystem> r = create_thread_pool(ThreadPoolDesc{.threads = 3});
    KILN_REQUIRE(r.ok());
    JobSystem jobs = r.value();
    KILN_CHECK_EQ(thread_pool_thread_count(jobs), u32(3));
    destroy_thread_pool(jobs);
}

KILN_TEST(IoThreadPool, ThreadCountDefaultIsClampedToRange) {
    Result<JobSystem> r = create_thread_pool(ThreadPoolDesc{});
    KILN_REQUIRE(r.ok());
    JobSystem jobs = r.value();
    u32 const n    = thread_pool_thread_count(jobs);
    KILN_CHECK(n >= 1 && n <= 16);
    destroy_thread_pool(jobs);
}

KILN_TEST(IoThreadPool, DestroyIdlePool) {
    Result<JobSystem> r = create_thread_pool(ThreadPoolDesc{});
    KILN_REQUIRE(r.ok());
    destroy_thread_pool(r.value()); // nothing submitted; must not hang
}

KILN_TEST(IoThreadPool, DestroyBusyPoolDrainsFirst) {
    Result<JobSystem> r = create_thread_pool(ThreadPoolDesc{.threads = 2, .queueCapacity = 64});
    KILN_REQUIRE(r.ok());
    JobSystem jobs = r.value();

    std::atomic<int> counter{0};
    constexpr int kJobs = 50;
    submit_n(jobs, &slow_increment_job, &counter, kJobs);
    destroy_thread_pool(jobs); // no wait_idle: destroy must drain before returning

    KILN_CHECK_EQ(counter.load(), kJobs);
}

namespace {

bool write_file(char const* path, Span<u8 const> bytes) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool const ok = bytes.size == 0 || std::fwrite(bytes.data, 1, bytes.size, f) == bytes.size;
    std::fclose(f);
    return ok;
}

/// Byte i == i mod 251. The prime period avoids aliasing, so any offset is checkable.
void fill_pattern(Vec<u8>& v, usize n) {
    v.resize(n);
    for (usize i = 0; i < n; ++i)
        v[i] = u8(i % 251);
}

} // namespace

KILN_TEST(IoCompat, ReadRangesMiddleAndEnd) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return;

    char path[1024];
    format(path, sizeof path, "%s/io_ranges.bin", dir);

    Vec<u8> pattern(default_allocator(), Tag::Test);
    fill_pattern(pattern, 4096);
    KILN_REQUIRE(write_file(path, pattern.span()));

    IoBackend const* io = compat_io_backend();
    IoFile f{};
    KILN_REQUIRE(io->open(io->user, StrView(path), &f).ok());
    KILN_CHECK(f.valid());

    u64 size = 0;
    KILN_REQUIRE(io->size(io->user, f, &size).ok());
    KILN_CHECK_EQ(size, u64(pattern.size()));

    // Middle range.
    u8 mid[128];
    KILN_REQUIRE(io->read_range(io->user, f, 2000, sizeof mid, mid).ok());
    KILN_CHECK(std::memcmp(mid, pattern.data() + 2000, sizeof mid) == 0);

    // Range ending exactly at EOF.
    u8 tail[64];
    u64 const tailOffset = pattern.size() - sizeof tail;
    KILN_REQUIRE(io->read_range(io->user, f, tailOffset, sizeof tail, tail).ok());
    KILN_CHECK(std::memcmp(tail, pattern.data() + tailOffset, sizeof tail) == 0);

    io->close(io->user, f);
}

namespace {
struct ReadJob {
    IoBackend const* io;
    IoFile file;
    u64 offset;
    u64 size;
    u8* dst;
    Status result;
};

void run_read_job(void* arg) {
    auto* j   = static_cast<ReadJob*>(arg);
    j->result = j->io->read_range(j->io->user, j->file, j->offset, j->size, j->dst);
}
} // namespace

KILN_TEST(IoCompat, ConcurrentReadRangeFromPoolThreads) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return;

    char path[1024];
    format(path, sizeof path, "%s/io_concurrent.bin", dir);

    Vec<u8> pattern(default_allocator(), Tag::Test);
    fill_pattern(pattern, 4096);
    KILN_REQUIRE(write_file(path, pattern.span()));

    IoBackend const* io = compat_io_backend();
    IoFile f{};
    KILN_REQUIRE(io->open(io->user, StrView(path), &f).ok());

    Result<JobSystem> r = create_thread_pool(ThreadPoolDesc{.threads = 4});
    KILN_REQUIRE(r.ok());
    JobSystem jobs = r.value();

    constexpr u32 kParts    = 4;
    constexpr u64 kPartSize = 1024;
    u8 dst[kParts][kPartSize];
    ReadJob readJobs[kParts];
    for (u32 i = 0; i < kParts; ++i) {
        readJobs[i] = ReadJob{io, f, u64(i) * kPartSize, kPartSize, dst[i], kOk};
        jobs.submit(jobs.user, &run_read_job, &readJobs[i]);
    }
    jobs.wait_idle(jobs.user);

    for (u32 i = 0; i < kParts; ++i) {
        KILN_CHECK(readJobs[i].result.ok());
        KILN_CHECK(std::memcmp(dst[i], pattern.data() + usize(i) * kPartSize, kPartSize) == 0);
    }

    destroy_thread_pool(jobs);
    io->close(io->user, f);
}

KILN_TEST(IoCompat, ReadPastEofReturnsIoEof) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return;

    char path[1024];
    format(path, sizeof path, "%s/io_eof.bin", dir);
    u8 const payload[] = {1, 2, 3, 4};
    KILN_REQUIRE(write_file(path, Span<u8 const>(payload, sizeof payload)));

    IoBackend const* io = compat_io_backend();
    IoFile f{};
    KILN_REQUIRE(io->open(io->user, StrView(path), &f).ok());

    u8 dst[1];
    Status st = io->read_range(io->user, f, sizeof payload, 1, dst); // exactly at EOF
    KILN_CHECK(st.failed());
    KILN_CHECK(st.code == Code::IoEof);

    io->close(io->user, f);
}

KILN_TEST(IoCompat, OpenMissingFileReturnsNotFound) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return;

    char path[1024];
    format(path, sizeof path, "%s/io_does_not_exist_12345.bin", dir);

    IoBackend const* io = compat_io_backend();
    IoFile f{};
    Status st = io->open(io->user, StrView(path), &f);
    KILN_CHECK(st.failed());
    KILN_CHECK(st.code == Code::NotFound);
    KILN_CHECK(!f.valid());
}

KILN_TEST(IoCompat, IoReadFileWholeFile) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return;

    char path[1024];
    format(path, sizeof path, "%s/io_whole.bin", dir);

    Vec<u8> pattern(default_allocator(), Tag::Test);
    fill_pattern(pattern, 777);
    KILN_REQUIRE(write_file(path, pattern.span()));

    Vec<u8> got(default_allocator(), Tag::Test);
    Status st = io_read_file(compat_io_backend(), StrView(path), default_allocator(), &got);
    KILN_REQUIRE(st.ok());
    KILN_REQUIRE_EQ(got.size(), pattern.size());
    KILN_CHECK(std::memcmp(got.data(), pattern.data(), pattern.size()) == 0);
}

KILN_TEST(IoCompat, FileExistsTrueFalseAndDirectory) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return;

    char path[1024];
    format(path, sizeof path, "%s/io_exists.bin", dir);
    u8 const one = 1;
    KILN_REQUIRE(write_file(path, Span<u8 const>(&one, 1)));

    KILN_CHECK(io_file_exists(StrView(path)));

    char missing[1024];
    format(missing, sizeof missing, "%s/io_exists_missing.bin", dir);
    KILN_CHECK(!io_file_exists(StrView(missing)));

    KILN_CHECK(!io_file_exists(StrView(dir))); // a directory is not a regular file
}

KILN_TEST(IoCompat, NonAsciiPathRoundTrips) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return;

    // "tést_ünïcode.bin" as UTF-8 bytes. The literal breaks after \xAF because a hex
    // escape would also eat the hex digit 'c' of "code".
    char path[1024];
    format(path, sizeof path,
           "%s/t\xC3\xA9st_\xC3\xBCn\xC3\xAF"
           "code.bin",
           dir);

    u8 const payload[] = {'k', 'i', 'l', 'n'};
#if defined(KILN_OS_WINDOWS)
    // Narrow fopen() uses the ANSI code page, not UTF-8, so write the fixture through the
    // wide API. io_read_file / io_file_exists below exercise kiln's own UTF-8 -> UTF-16 path.
    wchar_t wpath[1024];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, int(countof(wpath)));
    KILN_REQUIRE(wlen > 0);
    std::FILE* f = _wfopen(wpath, L"wb");
    KILN_REQUIRE(f != nullptr);
    KILN_REQUIRE(std::fwrite(payload, 1, sizeof payload, f) == sizeof payload);
    std::fclose(f);
#else
    KILN_REQUIRE(write_file(path, Span<u8 const>(payload, sizeof payload)));
#endif

    KILN_CHECK(io_file_exists(StrView(path)));

    Vec<u8> got(default_allocator(), Tag::Test);
    Status st = io_read_file(compat_io_backend(), StrView(path), default_allocator(), &got);
    KILN_REQUIRE(st.ok());
    KILN_REQUIRE_EQ(got.size(), sizeof payload);
    KILN_CHECK(std::memcmp(got.data(), payload, sizeof payload) == 0);
}
