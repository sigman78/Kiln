#include "kiln_test.h"

#include "kiln/assets.h" // StoreProfile
#include "kiln/cook/cook.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

using namespace kiln;
using namespace kiln::cook;

namespace {

/// An empty `<sample_dir()>/<suffix>`.
void scratch_dir(char const* suffix, char* out, usize cap) {
    format(out, cap, "%s/%s", kiln::test::sample_dir(), suffix);
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
}

struct DiagCapture {
    u32 code = 0;
    static void fn(void* user, Diagnostic const& d) noexcept {
        static_cast<DiagCapture*>(user)->code = d.code;
    }
    DiagSink sink() noexcept { return DiagSink{&fn, this}; }
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

// store_write / store_exists tests write under sample_dir().

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

    char storeDir[1024];
    format(storeDir, sizeof storeDir, "%s/store", dir);

    char name[64];
    usize n = store_file_name(store_key(100, 200, 300, kCookerVersion), "bin", name, sizeof name);
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

KILN_TEST(Store, ParseStoreProfile) {
    auto const parse = [](char const* text, StoreProfile* out) {
        return parse_store_profile(Span<u8 const>(reinterpret_cast<u8 const*>(text), std::strlen(text)), out)
            .code;
    };
    StoreProfile p;
    KILN_REQUIRE_EQ(parse("kiln-store 1\r\nprofile compat\nhash 00ff00ff00ff00ff\nformats 137 145\n", &p),
                    Code::Ok);
    KILN_CHECK(StrView(p.name) == "compat");
    KILN_CHECK_EQ(p.hash, u64(0x00ff00ff00ff00ffull));
    KILN_CHECK_EQ(p.blockFormats, block_format_bit(Format::BC3_UNORM) | block_format_bit(Format::BC7_UNORM));
    KILN_CHECK_EQ(parse("kiln-store 1\nprofile u\nhash 1\nformats\n", &p), Code::Ok); // uncompressed
    KILN_CHECK_EQ(p.blockFormats, u64(0));
    KILN_CHECK_EQ(parse("kiln-store 2\nprofile x\nhash 1\nformats\n", &p), Code::ParseError); // version
    KILN_CHECK_EQ(parse("kiln-store 1\nprofile x\nformats\n", &p), Code::ParseError);         // no hash
    KILN_CHECK_EQ(parse("kiln-store 1\nprofile x\nhash 1\nformats 37\n", &p),
                  Code::ParseError); // not a block format
    KILN_CHECK_EQ(parse("", &p), Code::ParseError);
}

KILN_TEST(Store, BindStoreProfile) {
    char dir[1024];
    scratch_dir("store_profile", dir, sizeof dir);
    // A fresh store gets the descriptor; the same profile binds again.
    KILN_REQUIRE(bind_store_profile(StrView(dir), kCompatTarget).ok());
    StoreProfile p;
    KILN_REQUIRE(read_store_profile(nullptr, StrView(dir), &p).ok());
    KILN_CHECK(StrView(p.name) == "compat");
    KILN_CHECK_EQ(p.hash, hash_target(kCompatTarget));
    KILN_CHECK_EQ(p.blockFormats, kCompatBlockFormats);
    KILN_CHECK(bind_store_profile(StrView(dir), kCompatTarget).ok());
    // Another profile writes nothing.
    DiagCapture cap;
    DiagSink sink = cap.sink();
    KILN_CHECK_EQ(bind_store_profile(StrView(dir), kDesktopTarget, &sink).code, Code::InvalidArgument);
    KILN_CHECK_EQ(cap.code, u32(kDiagStoreProfileMismatch));
    KILN_REQUIRE(read_store_profile(nullptr, StrView(dir), &p).ok());
    KILN_CHECK(StrView(p.name) == "compat");

    // A store with cooked files and no descriptor was cooked before profiles.
    char legacy[1024];
    scratch_dir("store_profile_legacy", legacy, sizeof legacy);
    u8 const bytes[4] = {1, 2, 3, 4};
    KILN_REQUIRE(store_write(StrView(legacy), "old.ktx2", Span<u8 const>(bytes, 4)).ok());
    DiagCapture cap2;
    DiagSink sink2 = cap2.sink();
    KILN_CHECK_EQ(bind_store_profile(StrView(legacy), kCompatTarget, &sink2).code, Code::InvalidArgument);
    KILN_CHECK_EQ(cap2.code, u32(kDiagStoreProfileMismatch));
    KILN_CHECK_EQ(read_store_profile(nullptr, StrView(legacy), &p).code, Code::NotFound);
}
