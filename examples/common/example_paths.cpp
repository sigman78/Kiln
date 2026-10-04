// examples/common/example_paths.cpp — finds an example's assets and store from where its
// executable is, so a copied tree runs as the build tree does.
#include "example_app.h"

#include <cstring>

#if defined(_WIN32)
#include <windows.h> // GetModuleFileNameW; WIN32_LEAN_AND_MEAN/NOMINMAX set by kiln_apply_defaults
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kiln::ex {
namespace {

constexpr u32 kMaxPath = 512;
constexpr u32 kMaxSubs = 4;

char g_exeDir[kMaxPath];
char g_assets[kMaxPath];
char g_store[kMaxPath];
char g_subs[kMaxSubs][kMaxPath];
u32 g_subCount = 0;

bool dir_exists(char const* path) {
#if defined(_WIN32)
    wchar_t wide[kMaxPath];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, int(kMaxPath))) return false;
    DWORD const attributes = GetFileAttributesW(wide);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

void find_exe_dir() {
    char path[kMaxPath] = {};
#if defined(_WIN32)
    wchar_t wide[kMaxPath];
    DWORD const n = GetModuleFileNameW(nullptr, wide, kMaxPath);
    if (n == 0 || n == kMaxPath ||
        !WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, int(kMaxPath), nullptr, nullptr))
        path[0] = 0;
#elif defined(__APPLE__)
    uint32_t size = kMaxPath;
    if (_NSGetExecutablePath(path, &size) != 0) path[0] = 0;
#else
    ssize_t const n     = readlink("/proc/self/exe", path, kMaxPath - 1);
    path[n > 0 ? n : 0] = 0;
#endif
    char* cut = nullptr;
    for (char* p = path; *p; ++p) {
        if (*p == '\\') *p = '/';
        if (*p == '/') cut = p;
    }
    if (cut) *cut = 0;
    format(g_exeDir, sizeof g_exeDir, "%s", cut ? path : ".");
}

/// `<dir>/<name>` for the nearest `dir`, from the executable's directory upwards, that has it.
bool find_upwards(char const* name, char* out) {
    char dir[kMaxPath];
    format(dir, sizeof dir, "%s", exe_dir());
    for (;;) {
        format(out, kMaxPath, "%s/%s", dir, name);
        if (dir_exists(out)) return true;
        char* const cut = std::strrchr(dir, '/');
        if (!cut) return false;
        *cut = 0;
    }
}

} // namespace

char const* exe_dir() {
    if (!g_exeDir[0]) find_exe_dir();
    return g_exeDir;
}

char const* asset_dir(char const* sub) {
    // A build tree outside the source tree finds nothing upwards: the path CMake knew.
    if (!g_assets[0] && !find_upwards("examples/assets", g_assets))
        format(g_assets, sizeof g_assets, "%s", KILN_EXAMPLE_ASSETS_DIR);
    KILN_VERIFY(g_subCount < kMaxSubs);
    char* const out = g_subs[g_subCount++];
    format(out, kMaxPath, "%s/%s", g_assets, sub);
    return out;
}

char const* store_dir() {
    if (!g_store[0] && !find_upwards("example-store", g_store))
        format(g_store, sizeof g_store, "%s/example-store", exe_dir());
    return g_store;
}

} // namespace kiln::ex
