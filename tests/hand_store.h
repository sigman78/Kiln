// tests/hand_store.h — stores written by hand, without cook code, so the reader-only (shipping)
// suite and kiln-headless read test files the way they read any store.
#pragma once

#include "kiln_test.h"

#include "kiln/assets.h"
#include "kiln/manifest.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace kiln::test {

/// A store at a scratch directory with one profile, `compat`. Every put() writes the artifact and
/// rewrites the manifest by a temporary file and a rename, so a store poller never reads half of it.
class HandStore {
public:
    /// An empty store at `<sample_dir()>/<name>`. With `clear`, what was there is removed first.
    bool init(char const* name, bool clear = true) {
        format(dir_, sizeof dir_, "%s/%s", sample_dir(), name);
        std::error_code ec;
        if (clear) std::filesystem::remove_all(dir_, ec);
        std::filesystem::create_directories(dir_, ec);
        return KILN_CHECK_MSG(std::filesystem::is_directory(dir_, ec), "cannot create %s", dir_);
    }
    char const* dir() const { return dir_; }
    /// The profile's block formats the manifest records (none by default: create() checks none).
    void set_block_formats(u64 formats) { blockFormats_ = formats; }

    /// The entry (`name`, `kind`) names an artifact holding `bytes` (added or replaced).
    bool put(StrView name, AssetKind kind, Span<u8 const> bytes) {
        Hash128 const key = xxh3_128(bytes);
        char path[1200];
        (void)artifact_file_path(StrView(dir_), key, path, sizeof path);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        if (!std::filesystem::exists(path, ec) && !write_atomic(path, bytes)) return false;
        Item* it = find(name, kind);
        if (!it) {
            if (!KILN_CHECK(count_ < kMaxItems && name.size < sizeof it->name)) return false;
            it = &items_[count_++];
            std::memcpy(it->name, name.data, name.size);
            it->nameLen = u32(name.size);
            it->kind    = kind;
        }
        it->key   = key;
        it->bytes = bytes.size;
        return write_manifest();
    }
    /// The entry (`name`, `kind`) leaves the manifest; its artifact stays, as after a kiln-cook drop.
    bool remove(StrView name, AssetKind kind) {
        Item* it = find(name, kind);
        if (!KILN_CHECK(it != nullptr)) return false;
        *it = items_[--count_];
        return write_manifest();
    }
    /// put() with the bytes of the file `path`.
    bool put_file(StrView name, AssetKind kind, char const* path) {
        Vec<u8> bytes(default_allocator(), Tag::Test);
        if (!KILN_CHECK_MSG(read(path, bytes), "cannot read %s", path)) return false;
        return put(name, kind, bytes.span());
    }

private:
    static constexpr usize kMaxItems = 64;
    struct Item {
        char name[256] = {};
        u32 nameLen    = 0;
        AssetKind kind = AssetKind::Mesh;
        Hash128 key;
        u64 bytes = 0;
        StrView view() const { return {name, nameLen}; }
    };

    Item* find(StrView name, AssetKind kind) {
        for (usize i = 0; i < count_; ++i)
            if (items_[i].kind == kind && items_[i].view() == name) return &items_[i];
        return nullptr;
    }

    static bool read(char const* path, Vec<u8>& out) {
        std::FILE* f = std::fopen(path, "rb");
        if (!f) return false;
        u8 buf[4096];
        for (usize n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
            out.append(Span<u8 const>(buf, n));
        std::fclose(f);
        return true;
    }

    /// A temporary file unique to this process and moment, then a rename over `path`.
    static bool write_atomic(char const* path, Span<u8 const> bytes) {
        char tmp[1300];
        format(tmp, sizeof tmp, "%s.%llx.tmp", path,
               static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::FILE* f = std::fopen(tmp, "wb");
        if (!KILN_CHECK_MSG(f != nullptr, "cannot write %s", tmp)) return false;
        bool const ok = std::fwrite(bytes.data, 1, bytes.size, f) == bytes.size;
        std::fclose(f);
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        return KILN_CHECK_MSG(ok && !ec, "cannot write %s", path);
    }

    static int compare(Item const* a, Item const* b) {
        usize const n = min(a->nameLen, b->nameLen);
        if (int const c = n ? std::memcmp(a->name, b->name, n) : 0; c != 0) return c;
        if (a->nameLen != b->nameLen) return a->nameLen < b->nameLen ? -1 : 1;
        return int(a->kind) - int(b->kind);
    }

    /// Format 0.1 (docs/design/store-manifest.md): header, the one profile, its entries sorted by
    /// name then kind, the index sorted by (name hash, entry), then the profile name and the entry
    /// names.
    bool write_manifest() {
        Item const* order[kMaxItems];
        for (usize i = 0; i < count_; ++i)
            order[i] = &items_[i];
        std::sort(order, order + count_, [](Item const* a, Item const* b) { return compare(a, b) < 0; });

        StrView const profile = "compat";
        u64 stringBytes       = profile.size;
        for (usize i = 0; i < count_; ++i)
            stringBytes += order[i]->nameLen;
        u64 const profiles = kManifestHeaderBytes;
        u64 const entries  = profiles + kManifestProfileBytes;
        u64 const index    = entries + count_ * kManifestEntryBytes;
        u64 const strings  = index + count_ * kManifestIndexBytes;
        u64 const total    = (strings + stringBytes + 7) & ~u64(7);
        Vec<u8> man(default_allocator(), Tag::Test);
        man.resize(usize(total), u8(0));
        u8* b = man.data();
        write_unaligned<u32>(b, kManifestMagic);
        write_unaligned<u16>(b + 4, kManifestMajor);
        write_unaligned<u16>(b + 6, kManifestMinor);
        write_unaligned<u32>(b + 8, kManifestHeaderBytes);
        write_unaligned<u64>(b + 16, total);
        write_unaligned<u32>(b + 24, 1u);
        write_unaligned<u64>(b + 32, u64(count_));
        write_unaligned<u64>(b + 40, profiles);
        write_unaligned<u64>(b + 48, entries);
        write_unaligned<u64>(b + 56, index);
        write_unaligned<u64>(b + 64, strings);
        write_unaligned<u64>(b + 72, stringBytes);
        u8* p = b + profiles;
        write_unaligned<u32>(p + 4, u32(profile.size));
        write_unaligned<u64>(p + 16, blockFormats_);
        write_unaligned<u64>(p + 32, u64(count_));
        std::memcpy(b + strings, profile.data, profile.size);

        struct IndexRec {
            u64 hash, entry;
        };
        IndexRec idx[kMaxItems];
        u64 str = profile.size;
        for (usize i = 0; i < count_; ++i) {
            Item const& it = *order[i];
            u8* e          = b + entries + i * kManifestEntryBytes;
            write_unaligned<u32>(e, u32(str));
            write_unaligned<u32>(e + 4, it.nameLen);
            write_unaligned<u16>(e + 8, u16(it.kind == AssetKind::Mesh ? 1 : 2));
            std::memcpy(e + 16, it.key.bytes, 16);
            std::memcpy(e + 32, it.key.bytes, 16); // the key is the content hash here
            write_unaligned<u64>(e + 48, it.bytes);
            std::memcpy(b + strings + str, it.name, it.nameLen);
            str += it.nameLen;
            idx[i] = {hash_name(it.view()), u64(i)};
        }
        std::sort(idx, idx + count_, [](IndexRec const& x, IndexRec const& y) {
            return x.hash != y.hash ? x.hash < y.hash : x.entry < y.entry;
        });
        for (usize i = 0; i < count_; ++i) {
            write_unaligned<u64>(b + index + i * kManifestIndexBytes, idx[i].hash);
            write_unaligned<u64>(b + index + i * kManifestIndexBytes + 8, idx[i].entry);
        }
        Hash128 const check = xxh3_128(man.span()); // the checksum field is still zero
        std::memcpy(b + kManifestChecksumOffset, check.bytes, 16);

        char path[1200];
        (void)manifest_file_path(StrView(dir_), path, sizeof path);
        return write_atomic(path, man.span());
    }

    char dir_[1024] = {};
    Item items_[kMaxItems];
    usize count_      = 0;
    u64 blockFormats_ = 0;
};

/// The goldens as a manifest store at `<sample_dir()>/golden-store`: `mesh/<x>` for
/// tests/golden/mesh/<x>.mesh and `ktx2/<x>` for tests/golden/ktx2/<x>.ktx2. Built once per process;
/// processes that build it at the same time write the same files.
inline char const* golden_store_dir() {
    static HandStore store;
    static bool built = false;
    if (built) return store.dir();
    if (!store.init("golden-store", false)) return store.dir();
    for (char const* sub : {"mesh", "ktx2"}) {
        char dir[1024];
        format(dir, sizeof dir, "%s/%s", golden_dir(), sub);
        std::error_code ec;
        for (auto const& f : std::filesystem::directory_iterator(dir, ec)) {
            std::filesystem::path const p = f.path();
            if (p.extension() != (std::strcmp(sub, "mesh") == 0 ? ".mesh" : ".ktx2")) continue;
            char name[256];
            format(name, sizeof name, "%s/%s", sub, p.stem().string().c_str());
            (void)store.put_file(StrView(name),
                                 std::strcmp(sub, "mesh") == 0 ? AssetKind::Mesh : AssetKind::Texture,
                                 p.string().c_str());
        }
    }
    built = true;
    return store.dir();
}

} // namespace kiln::test
