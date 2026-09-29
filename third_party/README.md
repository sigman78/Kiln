# Third-party dependencies

The cook entries are used by `kiln_cook` via the static helper library
`kiln_third_party_cook` defined in `third_party/CMakeLists.txt`; the viewer entries are used
only by the example executables under `examples/viewer/`. None of these reach `kiln_runtime`,
the shipping install, or `kiln_cook`'s own installed/consumer-facing interface — see `docs/design/dependencies.md` and
`docs/design/shipping-split.md`.

| Dependency | Version/commit | License | Used by | Notes |
|---|---|---|---|---|
| [cgltf](https://github.com/jkuhlmann/cgltf) | `v1.15`, commit `360db1a95480fe102ae9c69b27c5d101167ff5ba` | MIT | `kiln_cook` (glTF import; `KILN_MESH=ON` only) | Vendored header `third_party/cgltf/cgltf.h` + `LICENSE`, compiled once via `third_party/cgltf/cgltf.c` (`#define CGLTF_IMPLEMENTATION`). |
| [MikkTSpace](https://github.com/mmikk/MikkTSpace) | `master` @ commit `3e895b49d05ea07e4c2133156cfa94369e19e409` | zlib | `kiln_cook` (tangent-space generation, reference implementation) | Vendored `mikktspace.c` / `mikktspace.h`. Upstream has no top-level `LICENSE` file; the zlib license text is copied from the header comment in `mikktspace.c` into `third_party/mikktspace/LICENSE.txt` (with a note). **Local change:** the seed rotation in `QuickSort` and `QuickSortEdges` shifted by 32 when `t == 0` (undefined behavior, found by UBSan); that term is now 0, which leaves the result `uSeed` as both x86 and ARM computed it, so the output is unchanged. Re-apply after an update. |
| [wuffs](https://github.com/google/wuffs) | release file `release/c/wuffs-v0.4.c` @ commit `ba25980637db55730c62b466f188fae33b9289f6` | Apache-2.0 (dual MIT/Apache-2.0 upstream; kiln takes the Apache-2.0 terms) | `kiln_cook` (PNG, JPEG, optional WebP decode) | Vendored single-file release `third_party/wuffs/wuffs-v0.4.c` + `LICENSE`. Compiled once via `third_party/wuffs/wuffs_impl.c`, which defines `WUFFS_IMPLEMENTATION` and includes the module set in `wuffs_modules.h` (see below) before the release file, so the compiled object stays small. |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | `v1.3`, commit `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` | MIT | `kiln_cook` (mesh optimization: vertex cache / overdraw / vertex fetch; `KILN_MESH=ON` only, not fetched otherwise) | `FetchContent`, pinned to the commit hash (never a floating tag/branch), declared in `third_party/CMakeLists.txt`. Options: `MESHOPT_BUILD_DEMO=OFF`, `MESHOPT_BUILD_GLTFPACK=OFF`, `MESHOPT_BUILD_SHARED_LIBS=OFF`, `MESHOPT_INSTALL=OFF` (kiln installs the target itself, into the `kilnCookTargets` export set, instead). `FETCHCONTENT_UPDATES_DISCONNECTED=ON` for reproducible offline rebuilds after the first fetch. |
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) | master of 2026-07-30, commit `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` | MIT or public domain (Unlicense), kiln takes MIT | `kiln_cook` (BC1/3/4/5 with `rgbcx`, BC7 with `bc7enc`); `kiln_tests` (`bc7decomp`, `rgbcx`'s decoders) | Vendored `rgbcx.cpp/.h`, `rgbcx_table4_small.h`, `bc7enc.cpp/.h`, `bc7decomp.cpp/.h` and `LICENSE` in `third_party/bc7enc_rdo/`, compiled into `kiln_third_party_cook` with `RGBCX_USE_SMALLER_TABLES=1` (PUBLIC: it changes `rgbcx.h`) and `-ffp-contract=off`. The large `rgbcx_table4.h` and the ISPC, RDO and PNG files of the repository are not vendored. |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `vulkan-sdk-1.4.357.0`, commit `e3b1eec08173d6b825cd3ac88c885a63b621504a` | Apache-2.0 / MIT | `kiln-viewer`, `kiln-vk-smoke` (examples only) | `FetchContent` in `examples/viewer/CMakeLists.txt`, `SYSTEM`; only when `KILN_BUILD_VIEWER=ON`. Never reaches a library target. |
| [volk](https://github.com/zeux/volk) | commit `7f46f79751d7e3b3a6df20e38d3e3986585bcdf4` (1.4.364) | MIT | `kiln-viewer`, `kiln-vk-smoke` (examples only) | `FetchContent`, `SYSTEM`; loads Vulkan entry points at runtime, no loader link. |
| [GLFW](https://github.com/glfw/glfw) | `3.5.1`, commit `70a9bb3881fe80fd483236e2b203cb451c6ecf40` | zlib | the windowed examples (`kiln-viewer`, `kiln-gl`, `kiln-gl-bindless`), fetched once in `examples/common/CMakeLists.txt` | `FetchContent`, `SYSTEM`, static, examples/tests/docs/install off, Wayland off (X11 only on Linux). |
| [sokol](https://github.com/floooh/sokol) | master of 2026-09-15, commit `2e75443dbd4940b5aa8d76a8e479f8e4b270b9a3` | zlib | `kiln-sokol` (example only) | `FetchContent`, headers only (`SOURCE_SUBDIR` points nowhere); the implementation is `examples/sokol/sokol_impl.c`, compiled without kiln's warnings. |
| [sokol-shdc](https://github.com/floooh/sokol-tools-bin) | sokol-tools-bin commit `11d0cf678105d614d675e6d9bd2aaf3eeff12f8c` (2026-08-29) | MIT | `kiln-sokol` build (shader compiler) | `file(DOWNLOAD)` of the host's binary with `EXPECTED_HASH SHA256` (win32, linux, osx, osx_arm64), in `examples/sokol/CMakeLists.txt`. |
| [NoGraphicsAPI](https://github.com/sebbbi/NoGraphicsAPI) | main of 2026-09-28, commit `ae017a2f545abc0847e546cc7e84139bf3cc4241` | MIT | `kiln-nga` (example only) | `FetchContent` of the sources (`SOURCE_SUBDIR` points nowhere); `src/NoGraphicsAPI.cpp` is compiled by `examples/nga/CMakeLists.txt` without exceptions or RTTI, quietly. |
| [Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader) | `vulkan-sdk-1.4.357.0`, commit `5f157b62e333c63260d05d81bf66faa216ab0fb8` | Apache-2.0 | `kiln-nga` (example only) | `FetchContent`, WSI support off on Linux (headless); the DLL is copied next to `kiln-nga` on Windows. |
| [Slang](https://github.com/shader-slang/slang) | release `v2026.18.3` | Apache-2.0 with LLVM exception | `kiln-nga` build (shader compiler) | `file(DOWNLOAD)` of `slang-2026.18.3-windows-x86_64.zip` or `-linux-x86_64-glibc-2.28.tar.gz` with `EXPECTED_HASH SHA256`, unpacked into the build tree. |

## wuffs: module selection

`third_party/wuffs/wuffs_modules.h` defines exactly these `WUFFS_CONFIG__MODULE__*`
macros. `wuffs_impl.c` (implementation) and `src/cook/image_decode.cpp` (declarations)
include it before the release file. PNG's filter/scanline layer sits on DEFLATE, which
needs ADLER32 for zlib checksums and CRC32 for PNG chunk checksums:

- `WUFFS_CONFIG__MODULES` (opt in to the module system instead of compiling everything)
- `WUFFS_CONFIG__MODULE__BASE`
- `WUFFS_CONFIG__MODULE__ADLER32`
- `WUFFS_CONFIG__MODULE__CRC32`
- `WUFFS_CONFIG__MODULE__DEFLATE`
- `WUFFS_CONFIG__MODULE__PNG`
- `WUFFS_CONFIG__MODULE__ZLIB`
- `WUFFS_CONFIG__MODULE__JPEG`
- `WUFFS_CONFIG__MODULE__VP8` and `WUFFS_CONFIG__MODULE__WEBP`, only with `KILN_WEBP=ON`
  (VP8 is the lossy WebP bitstream)

No other codec modules (GIF, BMP, ...) are compiled in.

## Retrieval commands used to vendor these

```sh
# cgltf: latest tagged release, then resolve the tag to a commit SHA
gh api repos/jkuhlmann/cgltf/releases/latest --jq .tag_name
gh api repos/jkuhlmann/cgltf/git/ref/tags/v1.15 --jq .object.sha   # -> annotated tag object
gh api repos/jkuhlmann/cgltf/git/tags/<tag-object-sha> --jq .object.sha  # -> commit 360db1a9...
curl -sL -o third_party/cgltf/cgltf.h  "https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.15/cgltf.h"
curl -sL -o third_party/cgltf/LICENSE  "https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.15/LICENSE"

# MikkTSpace: current master commit
gh api repos/mmikk/MikkTSpace/commits/master --jq .sha
curl -sL -o third_party/mikktspace/mikktspace.c "https://raw.githubusercontent.com/mmikk/MikkTSpace/3e895b49d05ea07e4c2133156cfa94369e19e409/mikktspace.c"
curl -sL -o third_party/mikktspace/mikktspace.h "https://raw.githubusercontent.com/mmikk/MikkTSpace/3e895b49d05ea07e4c2133156cfa94369e19e409/mikktspace.h"

# wuffs: newest v0.4 single-file release, and the commit that last touched it
gh api repos/google/wuffs/contents/release/c --jq '.[].name'
gh api "repos/google/wuffs/commits?path=release/c/wuffs-v0.4.c&per_page=1" --jq '.[0].sha'
curl -sL -o third_party/wuffs/wuffs-v0.4.c "https://raw.githubusercontent.com/google/wuffs/ba25980637db55730c62b466f188fae33b9289f6/release/c/wuffs-v0.4.c"
curl -sL -o third_party/wuffs/LICENSE      "https://raw.githubusercontent.com/google/wuffs/ba25980637db55730c62b466f188fae33b9289f6/LICENSE"

# bc7enc_rdo: the master commit, then the files the cooker and tests use
gh api repos/richgel999/bc7enc_rdo/commits/master --jq .sha
for f in rgbcx.cpp rgbcx.h rgbcx_table4_small.h bc7enc.cpp bc7enc.h bc7decomp.cpp bc7decomp.h LICENSE; do
  curl -sL -o third_party/bc7enc_rdo/$f "https://raw.githubusercontent.com/richgel999/bc7enc_rdo/b9438627eef73a1157e84201b6fa6eb2ffd6d9f0/$f"
done

# meshoptimizer: latest release tag resolved to a commit hash
gh api repos/zeux/meshoptimizer/releases/latest --jq .tag_name
gh api repos/zeux/meshoptimizer/git/ref/tags/v1.3 --jq .object   # lightweight tag -> commit 9e1f07b1...
```

Every vendored file's content was verified against the corresponding GitHub
Git blob SHA (`git hash-object <file>` matched `gh api .../contents/<path> --jq .sha`
for every file above) before being committed.

## Rules for adding a dependency

- **Pin versions.** Use `FetchContent` with a commit hash, or vendor the source directly. Never
  float on a branch or tag alone.
- **Build with our flags.** Dependencies are built with the same compiler flags as the rest of the
  project where possible (see `cmake/kiln_warnings.cmake`), not their own defaults.
- **Permissive license only.** MIT, BSD, zlib, Apache-2.0, or a similarly permissive license.
  Nothing copyleft.
- **Record it here.** Every dependency gets a row in the table above with its license and which
  kiln target(s) use it.
