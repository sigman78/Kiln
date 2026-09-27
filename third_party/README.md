# Third-party dependencies

All entries below are cook-only (`kiln_cook`, via the static helper library
`kiln_third_party_cook` defined in `third_party/CMakeLists.txt`). None of
these reach `kiln_runtime`, the shipping install, or `kiln_cook`'s own
installed/consumer-facing interface — see `docs/design/dependencies.md` and
`docs/design/shipping-split.md`.

| Dependency | Version/commit | License | Used by | Notes |
|---|---|---|---|---|
| [cgltf](https://github.com/jkuhlmann/cgltf) | `v1.15`, commit `360db1a95480fe102ae9c69b27c5d101167ff5ba` | MIT | `kiln_cook` (glTF import) | Vendored header `third_party/cgltf/cgltf.h` + `LICENSE`, compiled once via `third_party/cgltf/cgltf.c` (`#define CGLTF_IMPLEMENTATION`). |
| [MikkTSpace](https://github.com/mmikk/MikkTSpace) | `master` @ commit `3e895b49d05ea07e4c2133156cfa94369e19e409` | zlib | `kiln_cook` (tangent-space generation, reference implementation) | Vendored `mikktspace.c` / `mikktspace.h`. Upstream has no top-level `LICENSE` file; the zlib license text is copied from the header comment in `mikktspace.c` into `third_party/mikktspace/LICENSE.txt` (with a note). |
| [wuffs](https://github.com/google/wuffs) | release file `release/c/wuffs-v0.4.c` @ commit `ba25980637db55730c62b466f188fae33b9289f6` | Apache-2.0 (dual MIT/Apache-2.0 upstream; kiln takes the Apache-2.0 terms) | `kiln_cook` (PNG decode) | Vendored single-file release `third_party/wuffs/wuffs-v0.4.c` + `LICENSE`. Compiled once via `third_party/wuffs/wuffs_impl.c`, which defines `WUFFS_IMPLEMENTATION` and the PNG-only module set (see below) before including the release file, so the compiled object stays small. |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | `v1.3`, commit `9e1f07b159d3cb777f1c67ed31fc11fd117986f4` | MIT | `kiln_cook` (mesh optimization: vertex cache / overdraw / vertex fetch) | `FetchContent`, pinned to the commit hash (never a floating tag/branch), declared in `third_party/CMakeLists.txt`. Options: `MESHOPT_BUILD_DEMO=OFF`, `MESHOPT_BUILD_GLTFPACK=OFF`, `MESHOPT_BUILD_SHARED_LIBS=OFF`, `MESHOPT_INSTALL=OFF` (kiln installs the target itself, into the `kilnCookTargets` export set, instead). `FETCHCONTENT_UPDATES_DISCONNECTED=ON` for reproducible offline rebuilds after the first fetch. |

## wuffs: PNG-only module selection

`third_party/wuffs/wuffs_impl.c` defines exactly these `WUFFS_CONFIG__MODULE__*`
macros before including the release file, which is all PNG decode needs
(PNG's filter/scanline layer sits on DEFLATE, which needs ADLER32 for zlib
checksums and CRC32 for PNG chunk checksums):

- `WUFFS_CONFIG__MODULES` (opt in to the module system instead of compiling everything)
- `WUFFS_CONFIG__MODULE__BASE`
- `WUFFS_CONFIG__MODULE__ADLER32`
- `WUFFS_CONFIG__MODULE__CRC32`
- `WUFFS_CONFIG__MODULE__DEFLATE`
- `WUFFS_CONFIG__MODULE__PNG`
- `WUFFS_CONFIG__MODULE__ZLIB`

No other codec modules (GIF, BMP, JPEG, ...) are compiled in.

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
