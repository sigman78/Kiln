# Texture-only builds

**Status:** Decided (owner, 2026-09-28): as proposed, all open points as proposed. All three
rollout steps are implemented.
**Decides:** How a host uses kiln as a texture cooker and loader only: what a CMake option removes
from the cook side, how an adapter says it takes no meshes, and why the runtime keeps its mesh code.

## Summary

- A new CMake option **`KILN_MESH`** (default `ON`). With `OFF`, `kiln_cook` builds without the
  glTF importer and the mesh cooker, and the build does not fetch or compile cgltf, MikkTSpace or
  meshoptimizer.
- The public API does not change. `cook_mesh` stays declared and returns `Unsupported` with the
  new diagnostic **K1021** ("mesh cooking not built"; the note first proposed K2012, but
  K2xxx is the image range).
- A new adapter capability bit, **`kMeshes`**. An adapter without it never gets a mesh upload, and
  a mesh request fails at the call.
- The runtime has no mesh switch. Its mesh code has no third-party dependency. The question
  returns when `.mesh` codecs bring a decoder into the runtime (v0.6).

## What exists

| Part | Mesh-only content | Dependencies |
|---|---|---|
| `kiln_cook` | `gltf_import.cpp`, `mesh_cook.cpp` (about 2,500 lines); the glb paths of `provider.cpp` and `cli_main.cpp` | cgltf, MikkTSpace, meshoptimizer (fetched when CMake configures) |
| `kiln_cook` | `mesh_write.cpp` (the `.mesh` writer) | none |
| `kiln_runtime` | `mesh_read.cpp`, and mesh branches in `loader.cpp`, `registry.cpp`, `pump.cpp` | none |
| Adapter | `UploadKind::MeshPayload`, `MeshPayloadDesc`, `CopyConstraints::bufferOffsetAlign` | — |

meshoptimizer is used only by `mesh_cook.cpp`: vertex deduplication (`meshopt_generateVertexRemap`)
and the vertex cache, overdraw and vertex fetch passes. The runtime only names meshopt codec ids when
it validates a `.mesh` blob table; it decodes nothing.

## Decision

### 1. `KILN_MESH` on the cook side

- `KILN_MESH=OFF` needs `KILN_BUILD_COOK=ON` to mean anything; a shipping build ignores it.
- `third_party/CMakeLists.txt` then builds only wuffs: no `FetchContent` of meshoptimizer, no cgltf,
  no MikkTSpace.
- `gltf_import.cpp` and `mesh_cook.cpp` drop out of `kiln_cook`. A small `mesh_cook_off.cpp`
  defines `cook_mesh`, which reports K1021 and returns `Unsupported`.
- These stay in every build, because they need no dependency and keep code and data portable:
  - `mesh_write.cpp` (tests and hosts write `.mesh` files with it);
  - `MeshCookSettings`, `ProviderDesc::meshDefaults`, the `[mesh]` keys of a `.kiln` sidecar;
  - the mesh flags of `kiln-cook`, so build scripts work with both builds.
- `KILN_MESH` is a private compile definition of `kiln_cook`, never visible in `include/`.

### 2. What a texture-only cook does with a model

| Input | Result |
|---|---|
| `request_mesh` of a `.glb` / `.gltf` with cook-on-miss | the asset becomes Failed with K5002, whose message carries K1021 |
| a texture embedded in a model (`chair.glb#wood`) | the same: only the glTF importer can read it |
| `kiln-cook` given a `.glb` / `.gltf` | K1021 for that input; the other inputs still cook; exit code 3 (a failed cook) |
| a `.mesh` file already in the store | loads normally (the runtime is unchanged) |

### 3. `kMeshes` at the adapter boundary

- `AdapterCaps` gains `kMeshes = 1u << 3`: "the adapter accepts `UploadKind::MeshPayload`".
- Without the bit, kiln never calls `acquire` or `begin_upload` with `MeshPayload`, and
  `request_mesh` / `register_mesh` return a failed handle with K5004 (`Unsupported`), as a cube
  request does without `kCubeTextures`. Such an adapter can ignore `bufferOffsetAlign`.
- The bit is positive, like the texture shape bits. Existing adapters must add it; this is an API
  break, recorded in `CHANGELOG.md`. The null adapter and the example Vulkan adapter set it.
- This is independent of `KILN_MESH`: a texture-only renderer can use a full kiln build, and a
  texture-only cook can feed an adapter that takes meshes.

### 4. No runtime switch now

- The runtime mesh code is small and has no dependency, so removing it saves little.
- A switch needs `#if` in `loader.cpp`, `registry.cpp` and `pump.cpp`, and a second shipping
  configuration to build and test.
- `kMeshes` already gives a texture-only host what it needs: its adapter never handles meshes.
- Revisit in v0.6. The `.mesh` codecs (meshopt, zstd) put decoders into `kiln_runtime`. Then a
  texture-only shipping build would carry decoders it never uses, and an option such as
  `KILN_RUNTIME_MESH` (or one per codec) becomes worth its cost.

### 5. Tests, examples, CI

- `tests/CMakeLists.txt` lists the mesh-only test files (`test_mesh_cook.cpp`,
  `test_mesh_golden.cpp`) only when `KILN_MESH=ON`. Files that mix both kinds (`test_provider.cpp`)
  guard their mesh cases with the `KILN_MESH` definition. A missing corpus is still an error, not
  a skip.
- New tests:
  - with `OFF`: a glb request and a `glb#image` request fail with K1021 (`Provider.MeshCookNotBuilt`);
    `kiln-cook` on a glb reports K1021 and exits 3 (`Provider.CliMainWithoutMeshCook`);
  - with both: an adapter without `kMeshes` gets no mesh upload, and `request_mesh` fails with
    K5004 (`Runtime.AdapterWithoutMeshes`).
- The glb-based CTests (`example_headless_roots*`) and the `viewer-demo` target need
  `KILN_MESH=ON`. `kiln-viewer` and `kiln-headless` build either way: they still load `.mesh`
  files from a store.
- CI gains one job, `texture-only`: `linux-clang-debug` with `-DKILN_MESH=OFF`, warnings as
  errors. It also checks that the build tree has no `_deps/meshoptimizer-src`, so a stray fetch
  fails the job.

## Diagnostics

| Code | Severity | Meaning |
|---|---|---|
| K1021 | Error | Mesh cooking is not built (`KILN_MESH=OFF`): a model source or a texture embedded in one |

## Rollout

1. *(done)* The `KILN_MESH` option, the third-party split, `mesh_cook_off.cpp`, K1021, and the provider
   and `kiln-cook` paths, with their tests.
2. *(done)* `kMeshes`: the runtime checks, both example adapters and the null adapter, the tests, and the
   CHANGELOG migration note.
3. *(done)* The CI job, and updates to `dependencies.md`, `shipping-split.md` and the build options in the
   top-level README.

## Open points

All four were decided as proposed (owner, 2026-09-28).

1. **The bit's direction.** A positive `kMeshes` breaks every existing adapter until it sets the
   bit. An inverse `kNoMeshes` breaks nothing but reads against the other caps. Proposed:
   positive, since pre-1.0 breaks are allowed.
2. **A stub or no declaration.** Proposed: keep `cook_mesh` declared, returning `Unsupported`, so
   host code builds against both configurations without `#if`. The alternative, a link error, finds
   the problem earlier but forces hosts to mirror the option.
3. **A query.** Should `kiln_cook` expose `bool cook_has_meshes() noexcept` for tools and hosts
   that want to hide mesh options? Proposed: not now; K1021 is enough.
4. **The option name.** `KILN_MESH` matches `KILN_WEBP`. `KILN_MESH_COOK` is more exact, but the
   runtime question in section 4 may later want its own name anyway.
