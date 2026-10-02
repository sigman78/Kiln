// src/cook/store.cpp — atomic store file writes (temp file, then rename).
// Paths are UTF-8 in stack buffers (no <filesystem>, no <string>).
// TODO: Windows uses the narrow ("A") API; UTF-16 paths belong to the IO backend.
#include "kiln/cook/cook.h"

#include "kiln/hash.h" // hash_combine
#include "kiln/log.h"

#include <atomic>
#include <cerrno>
#include <cstdio>

#if defined(KILN_OS_WINDOWS)
#include <direct.h>  // _mkdir
#include <windows.h> // SetFileInformationByHandle; WIN32_LEAN_AND_MEAN/NOMINMAX set by kiln_apply_defaults
#else
#include <sys/stat.h> // mkdir
#include <unistd.h>   // getpid
#endif

namespace kiln::cook {

namespace {

#if defined(KILN_OS_WINDOWS)
/// Replaces `dst` with `tmp`; 0 or the Windows error. MoveFileEx fails while a reader holds `dst`
/// open, even with FILE_SHARE_DELETE; a POSIX-semantics rename (NTFS, Windows 10 1607+) does not.
/// Other volumes and systems fall back to MoveFileEx. Paths are in the ANSI code page, as before.
DWORD rename_over(char const* tmp, char const* dst) {
    wchar_t wtmp[1024], wrel[1024];
    if (!MultiByteToWideChar(CP_ACP, 0, tmp, -1, wtmp, 1024) ||
        !MultiByteToWideChar(CP_ACP, 0, dst, -1, wrel, 1024))
        return ERROR_FILENAME_EXCED_RANGE;
    alignas(FILE_RENAME_INFO) unsigned char buf[sizeof(FILE_RENAME_INFO) + 1024 * sizeof(wchar_t)];
    auto* info      = reinterpret_cast<FILE_RENAME_INFO*>(buf);
    DWORD const len = GetFullPathNameW(wrel, 1024, info->FileName, nullptr);
    if (len == 0 || len >= 1024) return ERROR_FILENAME_EXCED_RANGE;
    info->Flags          = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory  = nullptr;
    info->FileNameLength = len * sizeof(wchar_t);

    HANDLE const h =
        CreateFileW(wtmp, DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    BOOL const ok   = SetFileInformationByHandle(h, FileRenameInfoEx, info, DWORD(sizeof buf));
    DWORD const err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (ok) return 0;
    if (err != ERROR_INVALID_PARAMETER && err != ERROR_NOT_SUPPORTED && err != ERROR_INVALID_FUNCTION)
        return err;
    return MoveFileExW(wtmp, info->FileName, MOVEFILE_REPLACE_EXISTING) ? 0 : GetLastError();
}
#endif

/// Copies `s` into `out` (NUL-terminated). False if it would overflow `cap`.
[[nodiscard]] bool to_cstr(char* out, usize cap, StrView s) {
    if (s.size + 1 > cap) return false;
    std::memcpy(out, s.data, s.size);
    out[s.size] = '\0';
    return true;
}

/// Builds "<dir>/<name>" into `out` (NUL-terminated). False if it would overflow `cap`.
[[nodiscard]] bool join_path(char* out, usize cap, StrView dir, StrView name) {
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
[[nodiscard]] bool append_tmp_suffix(char* buf, usize cap, u64 id) {
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
u64 next_tmp_id() {
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
[[nodiscard]] bool ensure_dir(char const* dir) {
#if defined(KILN_OS_WINDOWS)
    if (_mkdir(dir) == 0) return true;
#else
    if (mkdir(dir, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

} // namespace

bool store_exists(StrView dir, StrView name) {
    char path[1024];
    if (!join_path(path, sizeof path, dir, name)) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

Status store_write(StrView dir, StrView name, Span<u8 const> bytes, DiagSink const* diag, bool overwrite) {
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
    renameErrno = int(rename_over(tmp, dst));
    renamed     = renameErrno == 0;
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
