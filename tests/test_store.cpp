#include "kiln_test.h"

#include "kiln/cook/cook.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

using namespace kiln;
using namespace kiln::cook;

namespace {

struct DiagCapture {
    u32 code = 0;
    static void fn(void* user, Diagnostic const& d) { static_cast<DiagCapture*>(user)->code = d.code; }
    DiagSink sink() { return DiagSink{&fn, this}; }
};

bool file_exists(char const* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

bool read_whole_file(char const* path, Vec<u8>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return false;
    }
    long const sz = std::ftell(f);
    if (sz < 0 || std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return false;
    }
    out.resize(usize(sz));
    usize const wantRead = usize(sz);
    usize const got      = wantRead ? std::fread(out.data(), 1, wantRead, f) : 0;
    std::fclose(f);
    return got == wantRead;
}

} // namespace

// store_write / store_exists tests write under sample_dir().

namespace {

/// Writes a one-off entry directly into `dir` so a later test can rely on `dir`
/// existing (store_write only creates one directory level per call).
void seed_dir(StrView dir, u64 uniqueTag) {
    char name[64];
    usize n = format(name, sizeof name, "seed_%llu.bin", static_cast<unsigned long long>(uniqueTag));
    KILN_REQUIRE(n > 0);
    u8 const byte = 0;
    KILN_REQUIRE(store_write(dir, StrView(name, n), Span<u8 const>(&byte, 1)).ok());
}

} // namespace

KILN_TEST(Store, WriteThenExists) {
    char const* dir = kiln::test::sample_dir();

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);

    char name[64];
    usize n = format(name, sizeof name, "file_1_2_3.bin");
    KILN_REQUIRE(n > 0);
    StrView const nameView(name, n);

    u8 const payload[] = {1, 2, 3, 4, 5};
    Status st          = store_write(StrView(storeDir), nameView, Span<u8 const>(payload, sizeof payload));
    KILN_CHECK(st.ok());
    KILN_CHECK(store_exists(StrView(storeDir), nameView));

    char path[1024];
    format(path, sizeof path, "%s/%s", storeDir, name);
    Vec<u8> onDisk(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_whole_file(path, onDisk));
    KILN_REQUIRE_EQ(onDisk.size(), sizeof payload);
    KILN_CHECK(std::memcmp(onDisk.data(), payload, sizeof payload) == 0);
}

KILN_TEST(Store, SecondWriteLeavesContentAddressedFileUntouched) {
    char const* dir = kiln::test::sample_dir();

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);

    char name[64];
    usize n = format(name, sizeof name, "file_10_20_30.bin");
    KILN_REQUIRE(n > 0);
    StrView const nameView(name, n);

    u8 const first[]  = {0xAA, 0xBB, 0xCC};
    u8 const second[] = {0x11, 0x22, 0x33, 0x44};

    KILN_REQUIRE(store_write(StrView(storeDir), nameView, Span<u8 const>(first, sizeof first)).ok());
    // Same key/name, different bytes: content-addressed, so this must be a no-op.
    KILN_REQUIRE(store_write(StrView(storeDir), nameView, Span<u8 const>(second, sizeof second)).ok());

    char path[1024];
    format(path, sizeof path, "%s/%s", storeDir, name);
    Vec<u8> onDisk(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_whole_file(path, onDisk));
    KILN_REQUIRE_EQ(onDisk.size(), sizeof first);
    KILN_CHECK(std::memcmp(onDisk.data(), first, sizeof first) == 0);
}

KILN_TEST(Store, NoTempFileNameSurvivesAWrite) {
    char const* dir = kiln::test::sample_dir();

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);

    char name[64];
    usize n = format(name, sizeof name, "file_100_200_300.bin");
    KILN_REQUIRE(n > 0);
    StrView const nameView(name, n);

    u8 const payload[] = {7, 7, 7};
    KILN_REQUIRE(store_write(StrView(storeDir), nameView, Span<u8 const>(payload, sizeof payload)).ok());

    // Best effort without <filesystem>: the real temp name has an unpredictable suffix,
    // but the naive "<name>.tmp" must not exist either way.
    char tmpPath[1024];
    format(tmpPath, sizeof tmpPath, "%s/%.*s.tmp", storeDir, KILN_SV(nameView));
    KILN_CHECK(!file_exists(tmpPath));
    KILN_CHECK(store_exists(StrView(storeDir), nameView));
}

KILN_TEST(Store, MissingDirIsCreated) {
    char const* dir = kiln::test::sample_dir();

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);
    seed_dir(StrView(storeDir), 999); // store_write only creates one level per call

    char subDir[1024];
    format(subDir, sizeof subDir, "%s/fresh_subdir", storeDir);

    char name[64];
    usize n = format(name, sizeof name, "file_1000_2000_3000.bin");
    KILN_REQUIRE(n > 0);
    StrView const nameView(name, n);

    u8 const payload[] = {9};
    Status st          = store_write(StrView(subDir), nameView, Span<u8 const>(payload, sizeof payload));
    KILN_CHECK(st.ok());
    KILN_CHECK(store_exists(StrView(subDir), nameView));
}

KILN_TEST(Store, WriteUnderRegularFileIsIoError) {
    char const* dir = kiln::test::sample_dir();

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);
    seed_dir(StrView(storeDir), 998); // so we can create a plain file inside it

    char blockerPath[1024];
    format(blockerPath, sizeof blockerPath, "%s/blocker.bin", storeDir);
    {
        std::FILE* f = std::fopen(blockerPath, "wb");
        KILN_REQUIRE(f != nullptr);
        u8 const one = 1;
        std::fwrite(&one, 1, 1, f);
        std::fclose(f);
    }

    char name[64];
    usize n = format(name, sizeof name, "file_1_1_1.bin");
    KILN_REQUIRE(n > 0);

    u8 const payload[] = {'x'};
    Status const st    = store_write(StrView(blockerPath), StrView(name, n), Span<u8 const>(payload, 1));
    KILN_CHECK(st.failed());
    KILN_CHECK(st.code == Code::IoError);
}

KILN_TEST(Store, OverwriteReplacesExistingFile) {
    char const* dir = kiln::test::sample_dir();

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);

    // A fixed (not content-addressed) name, as hot-reload re-cooks write them.
    StrView const nameView("overwrite_me.bin");
    char path[1024];
    format(path, sizeof path, "%s/%.*s", storeDir, KILN_SV(nameView));
    std::remove(path); // left over from an earlier run

    u8 const first[]  = {1, 2, 3};
    u8 const second[] = {4, 5, 6, 7};
    u8 const third[]  = {8, 9};

    KILN_REQUIRE(store_write(StrView(storeDir), nameView, Span<u8 const>(first, sizeof first)).ok());

    // Without overwrite the existing file wins.
    KILN_REQUIRE(store_write(StrView(storeDir), nameView, Span<u8 const>(second, sizeof second)).ok());
    Vec<u8> onDisk(default_allocator(), Tag::Test);
    KILN_REQUIRE(read_whole_file(path, onDisk));
    KILN_REQUIRE_EQ(onDisk.size(), sizeof first);
    KILN_CHECK(std::memcmp(onDisk.data(), first, sizeof first) == 0);

    // With overwrite the bytes are replaced.
    KILN_REQUIRE(
        store_write(StrView(storeDir), nameView, Span<u8 const>(third, sizeof third), nullptr, true).ok());
    KILN_REQUIRE(read_whole_file(path, onDisk));
    KILN_REQUIRE_EQ(onDisk.size(), sizeof third);
    KILN_CHECK(std::memcmp(onDisk.data(), third, sizeof third) == 0);
}

// ---------------------------------------------------------------------------
// Store profiles (docs/design/target-profiles.md)
// ---------------------------------------------------------------------------
