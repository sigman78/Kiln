// src/cook/store.cpp — content-hashed store: atomic writes, no index (v0.5).
// Paths are UTF-8 in stack buffers (no <filesystem>, no <string>).
// TODO: Windows uses the narrow ("A") API; UTF-16 paths belong to the IO backend.
#include "kiln/cook/cook.h"

#include "kiln/assets.h" // StoreProfile
#include "kiln/log.h"

#include <atomic>
#include <cerrno>
#include <cstdio>

#if defined(KILN_OS_WINDOWS)
#include <direct.h>  // _mkdir
#include <windows.h> // MoveFileExA; WIN32_LEAN_AND_MEAN/NOMINMAX set by kiln_apply_defaults
#else
#include <dirent.h>   // opendir
#include <sys/stat.h> // mkdir
#include <unistd.h>   // getpid
#endif

namespace kiln::cook {

u64 store_key(u64 sourceHash, u64 settingsHash, u64 targetHash, u32 cookerVersion) noexcept {
    return hash_combine(hash_combine(hash_combine(sourceHash, settingsHash), targetHash), u64(cookerVersion));
}

usize store_file_name(u64 key, StrView ext, char* out, usize cap) noexcept {
    constexpr usize kHexDigits = 16;
    usize const needed         = kHexDigits + 1 + ext.size + 1; // hex + '.' + ext + NUL
    if (out == nullptr || cap < needed) return 0;

    static constexpr char kHex[] = "0123456789abcdef";
    for (usize i = 0; i < kHexDigits; ++i) {
        u32 const shift = u32(kHexDigits - 1 - i) * 4;
        out[i]          = kHex[(key >> shift) & 0xFu];
    }
    out[kHexDigits] = '.';
    std::memcpy(out + kHexDigits + 1, ext.data, ext.size);
    out[kHexDigits + 1 + ext.size] = '\0';
    return kHexDigits + 1 + ext.size;
}

namespace {

/// Copies `s` into `out` (NUL-terminated). False if it would overflow `cap`.
[[nodiscard]] bool to_cstr(char* out, usize cap, StrView s) noexcept {
    if (s.size + 1 > cap) return false;
    std::memcpy(out, s.data, s.size);
    out[s.size] = '\0';
    return true;
}

/// Builds "<dir>/<name>" into `out` (NUL-terminated). False if it would overflow `cap`.
[[nodiscard]] bool join_path(char* out, usize cap, StrView dir, StrView name) noexcept {
    usize const need = dir.size + 1 + name.size + 1; // dir + '/' + name + NUL
    if (need > cap) return false;
    std::memcpy(out, dir.data, dir.size);
    out[dir.size] = '/';
    std::memcpy(out + dir.size + 1, name.data, name.size);
    out[dir.size + 1 + name.size] = '\0';
    return true;
}

/// Appends ".tmp.<16 hex digits>" to the NUL-terminated path in `buf`. False if
/// it would overflow `cap`.
[[nodiscard]] bool append_tmp_suffix(char* buf, usize cap, u64 id) noexcept {
    usize const len  = std::strlen(buf);
    usize const need = len + 5 + 16 + 1; // ".tmp." + hex + NUL
    if (need > cap) return false;
    static constexpr char kHex[] = "0123456789abcdef";
    char* p                      = buf + len;
    *p++                         = '.';
    *p++                         = 't';
    *p++                         = 'm';
    *p++                         = 'p';
    *p++                         = '.';
    for (usize i = 0; i < 16; ++i) {
        u32 const shift = u32(15 - i) * 4;
        *p++            = kHex[(id >> shift) & 0xFu];
    }
    *p = '\0';
    return true;
}

/// Process- and call-unique id for temp file names. It only has to avoid collisions
/// with other writers racing on the same store directory.
[[nodiscard]] u64 next_tmp_id() noexcept {
    static std::atomic<u64> counter{0};
#if defined(KILN_OS_WINDOWS)
    u64 const pid = u64(GetCurrentProcessId());
#else
    u64 const pid = u64(getpid());
#endif
    return hash_combine(pid, counter.fetch_add(1, std::memory_order_relaxed));
}

/// mkdir(dir), treating "already exists" as success. One level only: a missing
/// grandparent directory is a caller error (see store_write's doc comment).
[[nodiscard]] bool ensure_dir(char const* dir) noexcept {
#if defined(KILN_OS_WINDOWS)
    if (_mkdir(dir) == 0) return true;
#else
    if (mkdir(dir, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

/// mkdir for every missing directory of the NUL-terminated `path`, then `path` itself.
[[nodiscard]] bool ensure_dirs(char* path) noexcept {
    for (char* p = path + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') continue;
        if (p[-1] == ':') continue; // a drive letter: "C:/"
        char const c  = *p;
        *p            = '\0';
        bool const ok = ensure_dir(path);
        *p            = c;
        if (!ok) return false;
    }
    return ensure_dir(path);
}

bool has_ext(char const* name, char const* ext) noexcept {
    usize const n = std::strlen(name), e = std::strlen(ext);
    return n > e && std::strcmp(name + n - e, ext) == 0;
}

/// True if `dir` or a directory below it holds a `.mesh` or `.ktx2` file.
bool has_cooked_files(char const* dir, u32 depth = 0) noexcept {
    if (depth > 32) return false;
    char path[1024];
#if defined(KILN_OS_WINDOWS)
    if (format(path, sizeof path, "%s/*", dir) + 1 >= sizeof path) return false;
    WIN32_FIND_DATAA fd;
    HANDLE const h = FindFirstFileA(path, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        char const* n = fd.cFileName;
        if (std::strcmp(n, ".") == 0 || std::strcmp(n, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            found = format(path, sizeof path, "%s/%s", dir, n) + 1 < sizeof path &&
                    has_cooked_files(path, depth + 1);
        } else {
            found = has_ext(n, ".mesh") || has_ext(n, ".ktx2");
        }
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    DIR* d = opendir(dir);
    if (!d) return false;
    bool found = false;
    while (!found) {
        dirent const* e = readdir(d);
        if (!e) break;
        char const* n = e->d_name;
        if (std::strcmp(n, ".") == 0 || std::strcmp(n, "..") == 0) continue;
        if (format(path, sizeof path, "%s/%s", dir, n) + 1 >= sizeof path) continue;
        struct stat st{};
        if (stat(path, &st) != 0) continue;
        found = S_ISDIR(st.st_mode) ? has_cooked_files(path, depth + 1)
                                    : has_ext(n, ".mesh") || has_ext(n, ".ktx2");
    }
    closedir(d);
    return found;
#endif
}

} // namespace

Status bind_store_profile(StrView storeDir, TargetProfile const& target, DiagSink const* diag) noexcept {
    StrView const name = target.name;
    if (name.empty() || name.size >= sizeof(StoreProfile::name) || name.find(' ') != StrView::kNpos ||
        name.find('\n') != StrView::kNpos)
        return diagf(diag, make_status(Code::InvalidArgument), kDiagStoreProfileMismatch, Severity::Error,
                     storeDir, "store", "profile name '%.*s' must be 1 to 63 characters without spaces",
                     KILN_SV(name));
    u64 const hash = hash_target(target);
    StoreProfile have;
    Status const st = read_store_profile(nullptr, storeDir, &have);
    if (st.ok()) {
        if (have.hash == hash) return kOk;
        return diagf(
            diag, make_status(Code::InvalidArgument), kDiagStoreProfileMismatch, Severity::Error, storeDir,
            "store",
            "the store was cooked for profile '%s' (%016llx); this cook's profile is '%.*s' (%016llx). "
            "Use another store directory, or delete this one",
            have.name, static_cast<unsigned long long>(have.hash), KILN_SV(name),
            static_cast<unsigned long long>(hash));
    }
    if (st.code != Code::NotFound)
        return diagf(diag, st, kDiagStoreProfileMismatch, Severity::Error, storeDir, "store",
                     "%s is not a valid store profile; delete the store", kStoreProfileFile);

    char dir[1024];
    if (!to_cstr(dir, sizeof dir, storeDir))
        return diagf(diag, make_status(Code::InvalidArgument), 0, Severity::Error, storeDir, "store",
                     "store directory path too long");
    if (has_cooked_files(dir))
        return diagf(diag, make_status(Code::InvalidArgument), kDiagStoreProfileMismatch, Severity::Error,
                     storeDir, "store",
                     "the store has cooked files but no %s (cooked before target profiles): delete it once",
                     kStoreProfileFile);
    if (!ensure_dirs(dir)) {
        int const e = errno;
        return diagf(diag, make_status(Code::IoError, u16(e & 0xFFFF)), 0, Severity::Error, storeDir, "mkdir",
                     "could not create the store directory (errno %d)", e);
    }

    char text[1024];
    usize n = format(text, sizeof text, "kiln-store 1\nprofile %.*s\nhash %016llx\nformats", KILN_SV(name),
                     static_cast<unsigned long long>(hash));
    for (u32 v = u32(Format::BC1_RGB_UNORM); v <= u32(Format::ASTC_12x12_SRGB); ++v)
        if (target.blockFormats & block_format_bit(Format(v)))
            n += format(text + n, sizeof text - n, " %u", v);
    n += format(text + n, sizeof text - n, "\n");
    return store_write(storeDir, StrView(kStoreProfileFile),
                       Span<u8 const>(reinterpret_cast<u8 const*>(text), n), diag, true);
}

bool store_exists(StrView dir, StrView name) noexcept {
    char path[1024];
    if (!join_path(path, sizeof path, dir, name)) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

Status store_write(StrView dir, StrView name, Span<u8 const> bytes, DiagSink const* diag,
                   bool overwrite) noexcept {
    char dst[1024];
    if (!join_path(dst, sizeof dst, dir, name)) {
        return diagf(diag, make_status(Code::InvalidArgument), 0, Severity::Error, name, "store",
                     "store path too long for dir %.*s", KILN_SV(dir));
    }

    // Content-addressed: same name means same bytes.
    if (!overwrite && store_exists(dir, name)) return kOk;

    char dirBuf[1024];
    if (!to_cstr(dirBuf, sizeof dirBuf, dir)) {
        return diagf(diag, make_status(Code::InvalidArgument), 0, Severity::Error, name, "store",
                     "store directory path too long: %.*s", KILN_SV(dir));
    }
    if (!ensure_dir(dirBuf)) {
        int const e = errno;
        return diagf(diag, make_status(Code::IoError, u16(e & 0xFFFF)), 0, Severity::Error, name, "mkdir",
                     "could not create store directory %s (errno %d)", dirBuf, e);
    }

    char tmp[1024];
    usize const dstLen = std::strlen(dst);
    std::memcpy(tmp, dst, dstLen + 1);
    if (!append_tmp_suffix(tmp, sizeof tmp, next_tmp_id())) {
        return diagf(diag, make_status(Code::InvalidArgument), 0, Severity::Error, name, "store",
                     "temp file path too long: %s", dst);
    }

    std::FILE* f = std::fopen(tmp, "wb");
    if (!f) {
        int const e = errno;
        return diagf(diag, make_status(Code::IoError, u16(e & 0xFFFF)), 0, Severity::Error, name, "fopen",
                     "could not open temp file %s for write (errno %d)", tmp, e);
    }
    usize const written  = bytes.size ? std::fwrite(bytes.data, 1, bytes.size, f) : usize(0);
    int const writeErrno = written == bytes.size ? 0 : errno;
    int const flushErrno = (writeErrno == 0 && std::fflush(f) != 0) ? errno : 0;
    // A failed close may be a delayed write error: the file must not reach the store.
    int const closeErrno = std::fclose(f) != 0 ? errno : 0;
    if (writeErrno != 0 || flushErrno != 0 || closeErrno != 0) {
        int const e = writeErrno != 0 ? writeErrno : flushErrno != 0 ? flushErrno : closeErrno;
        std::remove(tmp);
        return diagf(diag, make_status(Code::IoError, u16(e & 0xFFFF)), 0, Severity::Error, name, "fwrite",
                     "could not write temp file %s: wrote %zu of %zu bytes (errno %d)", tmp, written,
                     bytes.size, e);
    }

    bool renamed    = false;
    int renameErrno = 0;
#if defined(KILN_OS_WINDOWS)
    renamed = MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!renamed) renameErrno = int(GetLastError());
#else
    renamed = std::rename(tmp, dst) == 0;
    if (!renamed) renameErrno = errno;
#endif
    if (!renamed) {
        // Another writer may have won the race for this content-addressed name
        // between our existence check above and this rename; that's fine. When
        // overwriting, an existing file is the old content, so the rename must succeed.
        if (!overwrite && store_exists(dir, name)) {
            std::remove(tmp);
            return kOk;
        }
        std::remove(tmp);
        return diagf(diag, make_status(Code::IoError, u16(renameErrno & 0xFFFF)), 0, Severity::Error, name,
                     "rename", "could not rename %s to %s (error %d)", tmp, dst, renameErrno);
    }
    return kOk;
}

} // namespace kiln::cook
