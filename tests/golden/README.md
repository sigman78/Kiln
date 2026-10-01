# Golden files

Committed cooker output, compared byte-for-byte against a fresh cook every test run
(`tests/test_mesh_golden.cpp`, `tests/test_texture_golden.cpp`). This is stricter than the
structural checks in `tests/test_mesh_cook.cpp` /
`tests/test_texture_cook.cpp` (part/material/texture counts, etc.): a golden diff catches
*any* change to the exact encoded bytes — quantization bit patterns, vertex order after
optimization, string table layout, KTX2 level packing — including changes the structural
checks don't look at. It also doubles as the project's determinism check: if two cooks of
the same input with the same settings ever produce different bytes, a golden test fails.

## Layout

- `mesh/<stem>.mesh` — one entry per `ok` row of `tests/corpus/gltf/manifest.txt`, cooked
  with `resolve_mesh(MeshCookSettings{}, TargetProfile{}, CookSession{})` (the library's
  default resolved settings). `<stem>` is the corpus file's name without directory or
  extension (e.g. `generated/cube_basic.glb` -> `cube_basic.mesh`). The default
  compression is `Meshopt`, so they also pin meshoptimizer's encoder bytes.
- `ktx2/<case>.ktx2` — three PNGs generated in-process by `tests/png_writer.h`
  (deterministic, no external files), one per texture usage the cooker treats
  differently: `color_srgb` (sRGB, mips), `normal` (renormalized), `height16` (16-bit,
  mips). KTX2 pass-through has no cooker output to pin, so it isn't covered here.
- `ktx2/color_zstd.ktx2` — a 64x64 smooth ramp, big and smooth enough that the cook keeps Zstd:
  it pins the Zstd encoder's frames. Every other texture golden is under 4 KiB, so the cook stores
  it plain (`kZstdMinSaving`).
- `ktx2/bc*.ktx2` — the BC encoders on the `desktop` target profile (all of BC): `bc7_color_srgb`,
  `bc5_normal`, `bc6h_hdr` (the default table), `bc1_high` and `bc4_mask_high` (explicit BC1,
  and `High` quality). They pin the vendored encoders' bytes on every compiler and OS in CI.
- `ktx2/bc7_alpha_{fast,normal,high}.ktx2` — BC7 at each quality (`bc7f` for Fast and Normal,
  `bc7enc` for High) on a 16x16 image with opaque and translucent blocks.

Total size: 29 files, ~55 KB (the corpus itself is tiny by design — see
`tests/corpus/gltf/generated/README.md`).

## Regenerating

Either:

```sh
cmake --preset <preset> -DKILN_UPDATE_GOLDEN=ON
cmake --build --preset <preset>
ctest --preset <preset> -R kiln_tests
```

or run the test binary directly:

```sh
build/<preset>/tests/kiln_tests --update-golden Golden
```

Both write the golden files (creating `mesh/` / `ktx2/` if needed) and print what was
written. Reconfigure with `-DKILN_UPDATE_GOLDEN=OFF` (or drop `--update-golden`) and rerun
to confirm the freshly-written goldens compare clean.

## When a golden changes

**A golden diff must be deliberate, never a side effect.** Before regenerating and
committing new goldens:

1. Understand *why* the bytes changed. `tests/test_mesh_golden.cpp`'s failure message
   gives the first differing byte offset, both file sizes, and (via `MeshView::open` on
   both files) which section the offset falls in — enough to tell a real encoding change
   from a platform/toolchain difference (which should not happen and is a bug to fix, not
   a golden to update).
2. If the cooked output is meant to differ for the *same* source bytes and the *same*
   settings, bump `kiln::cook::kCookerVersion` in `include/kiln/cook/cook.h` first (it's
   part of every store key, so this also invalidates any on-disk store built with the old
   cooker).
3. Regenerate, review the diff (these are small binary files — check sizes and, for
   `.mesh`, `kiln-info --blobs` — not just "it passed"), and commit goldens and the code
   change together.
