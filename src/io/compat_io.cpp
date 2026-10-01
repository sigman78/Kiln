// Compatibility IO backend over Win32 or POSIX file handles (docs/design/threading-and-io.md).
// Paths arrive as UTF-8 StrViews without a NUL, so each call copies them into a stack buffer.
// This is the one place kiln converts paths to UTF-16 for the Windows W APIs.
#include "kiln/io.h"

#include <cstring>

#if defined(KILN_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kiln {

namespace {

// Longest accepted path, including the NUL terminator this backend adds.
constexpr usize kMaxPath = 1024;

#if defined(KILN_OS_WINDOWS)

// Converts `path` into `wbuf` (NUL-terminated). Returns false on invalid UTF-8.
[[nodiscard]] bool utf8_to_wide(StrView path, wchar_t (&wbuf)[kMaxPath]) noexcept {
    if (path.size == 0) {
        wbuf[0] = L'\0';
        return true;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data, int(path.size), wbuf,
                                   int(kMaxPath) - 1);
    if (wlen <= 0) return false;
    wbuf[wlen] = L'\0';
    return true;
}

HANDLE native_handle(IoFile f) noexcept {
    return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(f.bits - 1));
}

Status compat_open(void*, StrView path, IoFile* out) {
    if (path.size >= kMaxPath) return make_status(Code::InvalidArgument);
    wchar_t wbuf[kMaxPath];
    if (!utf8_to_wide(path, wbuf)) return make_status(Code::InvalidArgument);

    // FILE_SHARE_DELETE lets a writer replace the file by rename while it is open here
    // (store rewrites under hot reload; docs/design/hot-reload.md).
    HANDLE h = CreateFileW(wbuf, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD const err = GetLastError();
        Code const code =
            (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? Code::NotFound : Code::IoError;
        return make_status(code, u16(err & 0xFFFFu));
    }
    out->bits = u64(reinterpret_cast<std::uintptr_t>(h)) + 1;
    return kOk;
}

Status compat_size(void*, IoFile f, u64* out) {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(native_handle(f), &sz))
        return make_status(Code::IoError, u16(GetLastError() & 0xFFFFu));
    *out = u64(sz.QuadPart);
    return kOk;
}

// Without FILE_FLAG_OVERLAPPED, ReadFile still reads at OVERLAPPED.Offset, ignores the
// shared file pointer and completes synchronously. This gives positional reads on one
// handle without true async IO (planned for the v0.9 backend).
Status compat_read_range(void*, IoFile f, u64 offset, u64 size, void* dst) {
    HANDLE h = native_handle(f);
    u8* p    = static_cast<u8*>(dst);
    u64 done = 0;
    while (done < size) {
        u64 const at  = offset + done;
        OVERLAPPED ov = {};
        ov.Offset     = DWORD(at & 0xFFFFFFFFu);
        ov.OffsetHigh = DWORD(at >> 32);

        DWORD const want = DWORD(min<u64>(size - done, u64(0xFFFFFFFFu)));
        DWORD got        = 0;
        if (!ReadFile(h, p + done, want, &got, &ov)) {
            DWORD const err = GetLastError();
            if (err == ERROR_HANDLE_EOF) return make_status(Code::IoEof);
            return make_status(Code::IoError, u16(err & 0xFFFFu));
        }
        if (got == 0) return make_status(Code::IoEof); // requested past EOF
        done += got;
    }
    return kOk;
}

void compat_close(void*, IoFile f) { CloseHandle(native_handle(f)); }

Status compat_stat(void*, StrView path, IoStat* out) {
    if (path.size >= kMaxPath) return make_status(Code::InvalidArgument);
    wchar_t wbuf[kMaxPath];
    if (!utf8_to_wide(path, wbuf)) return make_status(Code::InvalidArgument);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(wbuf, GetFileExInfoStandard, &fa)) {
        DWORD const err = GetLastError();
        Code const code =
            (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? Code::NotFound : Code::IoError;
        return make_status(code, u16(err & 0xFFFFu));
    }
    if (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return make_status(Code::NotFound);
    out->size = (u64(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    // FILETIME counts 100 ns intervals.
    out->mtimeNs = ((u64(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime) * 100;
    return kOk;
}

bool stat_is_regular_file(StrView path) noexcept {
    if (path.size >= kMaxPath) return false;
    wchar_t wbuf[kMaxPath];
    if (!utf8_to_wide(path, wbuf)) return false;
    DWORD const attrs = GetFileAttributesW(wbuf);
    if (attrs == INVALID_FILE_ATTRIBUTES) return false;
    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

#else // POSIX

int native_fd(IoFile f) noexcept { return int(f.bits - 1); }

Status compat_open(void*, StrView path, IoFile* out) {
    if (path.size >= kMaxPath) return make_status(Code::InvalidArgument);
    char buf[kMaxPath];
    std::memcpy(buf, path.data, path.size);
    buf[path.size] = '\0';

    int fd = ::open(buf, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int const e     = errno;
        Code const code = (e == ENOENT || e == ENOTDIR) ? Code::NotFound : Code::IoError;
        return make_status(code, u16(e & 0xFFFF));
    }
    out->bits = u64(fd) + 1;
    return kOk;
}

Status compat_size(void*, IoFile f, u64* out) {
    struct stat st{};
    if (::fstat(native_fd(f), &st) != 0) return make_status(Code::IoError, u16(errno & 0xFFFF));
    *out = u64(st.st_size);
    return kOk;
}

Status compat_read_range(void*, IoFile f, u64 offset, u64 size, void* dst) {
    int const fd = native_fd(f);
    u8* p        = static_cast<u8*>(dst);
    u64 done     = 0;
    while (done < size) {
        usize const want = usize(size - done);
        ssize_t got      = ::pread(fd, p + done, want, off_t(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return make_status(Code::IoError, u16(errno & 0xFFFF));
        }
        if (got == 0) return make_status(Code::IoEof); // requested past EOF
        done += u64(got);
    }
    return kOk;
}

void compat_close(void*, IoFile f) { ::close(native_fd(f)); }

Status compat_stat(void*, StrView path, IoStat* out) {
    if (path.size >= kMaxPath) return make_status(Code::InvalidArgument);
    char buf[kMaxPath];
    std::memcpy(buf, path.data, path.size);
    buf[path.size] = '\0';
    struct stat st{};
    if (::stat(buf, &st) != 0) {
        int const e     = errno;
        Code const code = (e == ENOENT || e == ENOTDIR) ? Code::NotFound : Code::IoError;
        return make_status(code, u16(e & 0xFFFF));
    }
    if (!S_ISREG(st.st_mode)) return make_status(Code::NotFound);
    out->size = u64(st.st_size);
#if defined(__APPLE__)
    out->mtimeNs = u64(st.st_mtimespec.tv_sec) * 1000000000u + u64(st.st_mtimespec.tv_nsec);
#else
    out->mtimeNs = u64(st.st_mtim.tv_sec) * 1000000000u + u64(st.st_mtim.tv_nsec);
#endif
    return kOk;
}

bool stat_is_regular_file(StrView path) noexcept {
    if (path.size >= kMaxPath) return false;
    char buf[kMaxPath];
    std::memcpy(buf, path.data, path.size);
    buf[path.size] = '\0';
    struct stat st{};
    if (::stat(buf, &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

#endif

constexpr IoBackend g_compatBackend{&compat_open,  &compat_size, &compat_read_range,
                                    &compat_close, &compat_stat, nullptr};

} // namespace

IoBackend const* compat_io_backend() noexcept { return &g_compatBackend; }

Status io_read_file(IoBackend const* io, StrView path, Allocator const* alloc, Vec<u8>* out) noexcept {
    KILN_ASSERT(io && io->open && io->size && io->read_range && io->close);
    KILN_ASSERT(out != nullptr);
    if (!out->allocator()) out->init(alloc ? alloc : default_allocator(), Tag::Io);

    IoFile f{};
    Status st = io->open(io->user, path, &f);
    if (st.failed()) return st;

    u64 size = 0;
    st       = io->size(io->user, f, &size);
    if (st.ok()) {
        out->resize(usize(size));
        if (size > 0) st = io->read_range(io->user, f, 0, size, out->data());
    }
    io->close(io->user, f);
    return st;
}

bool io_file_exists(StrView path) noexcept { return stat_is_regular_file(path); }

} // namespace kiln
