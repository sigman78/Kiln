# Changelog

All notable changes to this project are documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Pre-1.0: API breaks are allowed but every break is recorded here with migration notes.

## [Unreleased]

### Added
- M3: runtime (kiln/assets.h): context, handles with generations, states Unloaded/Pending/MetaReady/Ready/Failed, refcounted requests with two priorities, pump() with upload budget and Busy back-pressure, events, load groups with progress/wait (panics on misuse), kind-specific placeholders (host-overridable, dev magenta for Failed), publish/acquire adapter hooks and gpu() lookup, in-memory registration, cook-on-miss provider hook; compat IO backend (positional reads) and built-in thread pool (kiln/io.h); null adapter (kiln/null_adapter.h); kiln_cook install_provider (kiln/cook/provider.h). Zero steady-state allocations verified by tag stats. Diagnostics K5001-K5009.
- M3: cook-on-miss provider (`kiln/cook/provider.h`, `kiln::cook::install_provider`/`uninstall_provider`):
  installs a `CookProvider` on a runtime `Context` that cooks missing mesh/texture assets from the
  context's source roots (`.glb`/`.gltf`, `.png`/`.ktx2`), writing `StoreLayout::Named` files in Disk mode
  or staying cache-less in Memory mode; a texture with no source of its own is produced by cooking its
  owning mesh. Dev builds only (`kiln_cook`); `kiln_runtime` never references it.
- M2: cooker. glTF/GLB -> `.mesh` (`kiln/cook/cook.h`: cgltf import, naming conventions `mount_`/`_lodN`/`col_`/`_`, hierarchy parts with baked scale, MikkTSpace tangents, meshoptimizer vertex cache/overdraw/fetch, quantized default profile with float fallback, authored LOD pass-through, materials with `.NNN` stripping, texture bindings + UV sets, mount extras); PNG -> KTX2 (`kiln/cook/image.h`: wuffs decode incl. 16-bit, integer-exact sRGB mips, normal renormalization, size caps) and KTX2 pass-through; v0.5 settings subset with slot inference and field-wise hashing (`kiln/cook/settings.h`); content-hashed store with atomic writes; `kiln-cook` CLI (`--check`, `--named`, `--map`); glTF corpus (`tests/corpus/gltf/`, generated edge cases + Khronos samples) and golden files (`tests/golden/`); diagnostics K1001-K1018, K2001-K2008, K3001-K3004. Dependencies: cgltf 1.15, MikkTSpace, wuffs 0.4 (vendored), meshoptimizer 1.3 (FetchContent, pinned), cook side only.
- KTX2 real-world corpus (`tests/corpus/ktx2/`): curated Khronos KTX-Software test images (Apache-2.0) plus `ktx create`-generated variants, a manifest-driven reader test, writer round-trips with DFD byte comparison, and `ktx validate` CTest checks on every file kiln writes (CI installs KTX-Software 4.4.2). Format table gains RGB8, ETC2/EAC and ASTC LDR rows.
- M1.5: read-only shipping contract: cook headers moved to include/kiln/cook/, install components (runtime, cook), *-shipping presets, shipping CI job with a find_package consumer, reader-only test split.
- M1: `.mesh` reader (`kiln/mesh.h`: spec v0.3 records, `MeshView`, always-on header/BLOB checks, full validation, payload decode loop with codec None, checksum and index checks) and deterministic writer (`kiln/cook/mesh_writer.h`, kiln_cook); KTX2 raw reader (`kiln/ktx2.h`) and writer (`kiln/cook/ktx2_writer.h`); `Format` enum + constexpr table (`kiln/formats.h`, values == VkFormat); `kiln-info` tool; `docs/diagnostics.md` (K4000-K4199).
- Project display name is Kiln; GitHub repository renamed to sigman78/Kiln (namespace, CMake package and targets stay lowercase `kiln`).

### Fixed
- KTX2 writer: the alpha sample of sRGB formats carried the EXPONENT qualifier (0x20) instead of LINEAR (0x10); found by `ktx validate`.

### Changed
- `.mesh` writer: `WriteOptions::splitBytes` and `kiln::mesh::split_unit()` are removed (pre-1.0 break).
  The cooker never set them; the writer emits one blob per vertex stream per LOD and one per index
  range. Blob splitting returns with the v0.6 codec work, and spec §5.9 now records the split rules as
  deferred. Migration: drop the field; files written without splitting are unchanged.
- `kiln-cook` writes the `StoreLayout::Named` layout (`<store>/<assetPath>.<ext>`) by default, matching
  what the runtime's v0.5 store expects (`kiln/assets.h`); the old content-hashed file names move behind
  `--hashed`, kept for the index-based hashed layout landing in v0.6 (pre-1.0 break: scripts that relied
  on the previous default now need `--hashed`, or to switch to the named paths).
- Cooked KTX2 files carry `kiln.sourceHash` and `kiln.cookHash` key/value entries (the invalidation identity for the named store layout); the KTX2 writer accepts extra sorted key/value entries. Cooker version 2; goldens regenerated.
- C++23 idioms: `if consteval` in the hash readers, `std::unreachable()` instead of `KILN_UNREACHABLE`, `std::to_underlying` and `std::is_scoped_enum` for enum hashing/printing, `KILN_ASSUME`, and monadic `Result<T>` (`and_then`, `transform`, `or_else`).
- Language baseline raised from C++20 to C++23 (owner decision 2026-09-27): CMake `cxx_std_23`; minimum compilers MSVC 2022 17.10+, clang 17+, gcc 13+.
- Header paths: kiln/mesh_writer.h -> kiln/cook/mesh_writer.h, kiln/ktx2_writer.h -> kiln/cook/ktx2_writer.h (pre-1.0 break; update includes).

- M0: repository skeleton, CMake targets and presets, CI, core vocabulary types (allocator,
  Result/Status, panic, log, Span/StrView, FixedArray/Vec/HashMap, hash/fourcc), minimal test
  runner, design notes.
- Design notes aligned to HANDOFF v2 (blob table, MetaReady, kind placeholders, load groups,
  publish/acquire/caps, gpu()).
- mesh-format-spec v0.3: KMSH magic, kiln::mesh namespace, exact-minor version rule while 0.x, BLOB rules resolved (filter/codec combinations, split alignment, table order, raw fast path conditions, lodRank, size limits).
