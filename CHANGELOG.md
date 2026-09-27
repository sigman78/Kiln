# Changelog

All notable changes to this project are documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Pre-1.0: API breaks are allowed but every break is recorded here with migration notes.

## [Unreleased]

### Added
- M2: cooker. glTF/GLB -> `.mesh` (`kiln/cook/cook.h`: cgltf import, naming conventions `mount_`/`_lodN`/`col_`/`_`, hierarchy parts with baked scale, MikkTSpace tangents, meshoptimizer vertex cache/overdraw/fetch, quantized default profile with float fallback, authored LOD pass-through, materials with `.NNN` stripping, texture bindings + UV sets, mount extras); PNG -> KTX2 (`kiln/cook/image.h`: wuffs decode incl. 16-bit, integer-exact sRGB mips, normal renormalization, size caps) and KTX2 pass-through; v0.5 settings subset with slot inference and field-wise hashing (`kiln/cook/settings.h`); content-hashed store with atomic writes; `kiln-cook` CLI (`--check`, `--named`, `--map`); glTF corpus (`tests/corpus/gltf/`, generated edge cases + Khronos samples) and golden files (`tests/golden/`); diagnostics K1001-K1018, K2001-K2008, K3001-K3004. Dependencies: cgltf 1.15, MikkTSpace, wuffs 0.4 (vendored), meshoptimizer 1.3 (FetchContent, pinned), cook side only.
- KTX2 real-world corpus (`tests/corpus/ktx2/`): curated Khronos KTX-Software test images (Apache-2.0) plus `ktx create`-generated variants, a manifest-driven reader test, writer round-trips with DFD byte comparison, and `ktx validate` CTest checks on every file kiln writes (CI installs KTX-Software 4.4.2). Format table gains RGB8, ETC2/EAC and ASTC LDR rows.
- M1.5: read-only shipping contract: cook headers moved to include/kiln/cook/, install components (runtime, cook), *-shipping presets, shipping CI job with a find_package consumer, reader-only test split.
- M1: `.mesh` reader (`kiln/mesh.h`: spec v0.3 records, `MeshView`, always-on header/BLOB checks, full validation, payload decode loop with codec None, checksum and index checks) and deterministic writer (`kiln/cook/mesh_writer.h`, kiln_cook); KTX2 raw reader (`kiln/ktx2.h`) and writer (`kiln/cook/ktx2_writer.h`); `Format` enum + constexpr table (`kiln/formats.h`, values == VkFormat); `kiln-info` tool; `docs/diagnostics.md` (K4000-K4199).
- Project display name is Kiln; GitHub repository renamed to sigman78/Kiln (namespace, CMake package and targets stay lowercase `kiln`).

### Fixed
- KTX2 writer: the alpha sample of sRGB formats carried the EXPONENT qualifier (0x20) instead of LINEAR (0x10); found by `ktx validate`.

### Changed
- Language baseline raised from C++20 to C++23 (owner decision 2026-09-27): CMake `cxx_std_23`; minimum compilers MSVC 2022 17.10+, clang 17+, gcc 13+.
- Header paths: kiln/mesh_writer.h -> kiln/cook/mesh_writer.h, kiln/ktx2_writer.h -> kiln/cook/ktx2_writer.h (pre-1.0 break; update includes).

- M0: repository skeleton, CMake targets and presets, CI, core vocabulary types (allocator,
  Result/Status, panic, log, Span/StrView, FixedArray/Vec/HashMap, hash/fourcc), minimal test
  runner, design notes.
- Design notes aligned to HANDOFF v2 (blob table, MetaReady, kind placeholders, load groups,
  publish/acquire/caps, gpu()).
- mesh-format-spec v0.3: KMSH magic, kiln::mesh namespace, exact-minor version rule while 0.x, BLOB rules resolved (filter/codec combinations, split alignment, table order, raw fast path conditions, lodRank, size limits).
