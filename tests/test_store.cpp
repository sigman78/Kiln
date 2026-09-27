#include "kiln_test.h"

#include "kiln/cook/cook.h"

#include <cstdio>

using namespace kiln;
using namespace kiln::cook;

namespace {

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

// ---------------------------------------------------------------------------
// store_key
// ---------------------------------------------------------------------------

KILN_TEST(Store, KeyIsOrderSensitive) {
    u64 const a = 0x1111111111111111ull;
    u64 const b = 0x2222222222222222ull;
    u64 const c = 0x3333333333333333ull;

    u64 const k1 = store_key(a, b, c, 7);
    u64 const k2 = store_key(b, a, c, 7); // sourceHash/settingsHash swapped
    KILN_CHECK_NE(k1, k2);

    u64 const k3 = store_key(a, c, b, 7); // settingsHash/targetHash swapped
    KILN_CHECK_NE(k1, k3);

    u64 const k4 = store_key(a, b, c, 7);
    KILN_CHECK_EQ(k1, k4); // deterministic for identical inputs
}

KILN_TEST(Store, KeyIsCookerVersionSensitive) {
    u64 const a = 42, b = 43, c = 44;
    KILN_CHECK_NE(store_key(a, b, c, 1), store_key(a, b, c, 2));
    KILN_CHECK_EQ(store_key(a, b, c, kCookerVersion), store_key(a, b, c, kCookerVersion));
}

// ---------------------------------------------------------------------------
// store_file_name
// ---------------------------------------------------------------------------

KILN_TEST(Store, FileNameFormatting) {
    char buf[64];
    usize n = store_file_name(0x00000000deadbeefull, "mesh", buf, sizeof buf);
    KILN_REQUIRE(n > 0);
    KILN_CHECK_EQ(StrView(buf, n), StrView("00000000deadbeef.mesh"));
    KILN_CHECK_EQ(buf[n], '\0');

    usize n2 = store_file_name(0x0123456789abcdefull, "ktx2", buf, sizeof buf);
    KILN_REQUIRE(n2 > 0);
    KILN_CHECK_EQ(StrView(buf, n2), StrView("0123456789abcdef.ktx2"));
}

KILN_TEST(Store, FileNameCapacity) {
    // "00000000deadbeef.mesh" is 21 chars; 22 with the NUL.
    char tooSmall[21];
    KILN_CHECK_EQ(store_file_name(0x00000000deadbeefull, "mesh", tooSmall, sizeof tooSmall), usize(0));

    char exact[22];
    usize n = store_file_name(0x00000000deadbeefull, "mesh", exact, sizeof exact);
    KILN_CHECK_EQ(n, usize(21));
    KILN_CHECK_EQ(exact[21], '\0');
}

// ---------------------------------------------------------------------------
// store_write / store_exists
//
// These need a real directory (kiln::test::sample_dir(), --samples <dir>); they
// no-op when it wasn't given, same convention as test_ktx2.cpp's sample-file test.
// ---------------------------------------------------------------------------

namespace {

/// Writes a one-off entry directly into `dir` so a later test can rely on `dir`
/// existing (store_write only creates one directory level per call).
void seed_dir(StrView dir, u64 uniqueTag) {
    char name[64];
    usize n =
        store_file_name(store_key(uniqueTag, uniqueTag, uniqueTag, kCookerVersion), "bin", name, sizeof name);
    KILN_REQUIRE(n > 0);
    u8 const byte = 0;
    KILN_REQUIRE(store_write(dir, StrView(name, n), Span<u8 const>(&byte, 1)).ok());
}

} // namespace

KILN_TEST(Store, WriteThenExists) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return; // no --samples <dir>: nothing to do

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);

    char name[64];
    usize n = store_file_name(store_key(1, 2, 3, kCookerVersion), "bin", name, sizeof name);
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
    if (!dir) return;

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);

    char name[64];
    usize n = store_file_name(store_key(10, 20, 30, kCookerVersion), "bin", name, sizeof name);
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
    if (!dir) return;

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);

    char name[64];
    usize n = store_file_name(store_key(100, 200, 300, kCookerVersion), "bin", name, sizeof name);
    KILN_REQUIRE(n > 0);
    StrView const nameView(name, n);

    u8 const payload[] = {7, 7, 7};
    KILN_REQUIRE(store_write(StrView(storeDir), nameView, Span<u8 const>(payload, sizeof payload)).ok());

    // No <filesystem> here to enumerate the directory, so this is a best-effort
    // check: store_write's actual temp name carries a random/pid suffix we can't
    // predict, but the naive fixed pattern "<name>.tmp" must not exist either way.
    char tmpPath[1024];
    format(tmpPath, sizeof tmpPath, "%s/%.*s.tmp", storeDir, KILN_SV(nameView));
    KILN_CHECK(!file_exists(tmpPath));
    KILN_CHECK(store_exists(StrView(storeDir), nameView));
}

KILN_TEST(Store, MissingDirIsCreated) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return;

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);
    seed_dir(StrView(storeDir), 999); // store_write only creates one level per call

    char subDir[1024];
    format(subDir, sizeof subDir, "%s/fresh_subdir", storeDir);

    char name[64];
    usize n = store_file_name(store_key(1000, 2000, 3000, kCookerVersion), "bin", name, sizeof name);
    KILN_REQUIRE(n > 0);
    StrView const nameView(name, n);

    u8 const payload[] = {9};
    Status st          = store_write(StrView(subDir), nameView, Span<u8 const>(payload, sizeof payload));
    KILN_CHECK(st.ok());
    KILN_CHECK(store_exists(StrView(subDir), nameView));
}

KILN_TEST(Store, WriteUnderRegularFileIsIoError) {
    char const* dir = kiln::test::sample_dir();
    if (!dir) return;

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
    usize n = store_file_name(store_key(1, 1, 1, kCookerVersion), "bin", name, sizeof name);
    KILN_REQUIRE(n > 0);

    u8 const payload[] = {'x'};
    Status const st    = store_write(StrView(blockerPath), StrView(name, n), Span<u8 const>(payload, 1));
    KILN_CHECK(st.failed());
    KILN_CHECK(st.code == Code::IoError);
}
