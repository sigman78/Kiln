# Cook kernels: hot paths, SIMD and task splitting

Status: **Proposed** (2026-09-27). Owner asked for the review and approved the rollout below.

## Decision

The cooker's per-texel and per-vertex work is organized as **kernels**: small pure functions with
a fixed shape that the compilers can vectorize, that can be split across worker threads by rows or
by items, and whose output is byte-identical in every configuration. The rest of the cooker
(parsing, planning, interning, file writing) stays as it is.

### Where the time goes

- **Textures.** Every kernel in `src/cook/image.cpp` was a generic per-texel loop that branched on
  bit depth and channel count inside the inner loop and loaded bytes one channel at a time. No
  compiler vectorizes that. The single worst item was `linear16_to_srgb8`: an eight-step binary
  search per color channel per output texel, called four times per texel for sRGB mips. The texture
  path also made four to five full passes over level 0 (decode, convert, flip green, renormalize,
  box filter) before the first mip existed.
- **Meshes.** The cooker's own per-vertex loops are cheap next to the third-party stages
  (meshoptimizer, MikkTSpace, cgltf). SIMD does not help there. What is available is task
  parallelism: every (part, LOD) pair goes through build, weld, normals, tangents, optimize and pack
  independently; only the final interning into shared tables is order-dependent.
- Nothing was measured before this note. Measurement comes first in the rollout.

### Kernel contract

A kernel lives in `src/cook/kernels.h` (internal, never installed) and follows these rules:

1. **Fixed shape.** A kernel is a function over a row range or an item range:
   `kernel<Params...>(Ctx const& ctx, u32 begin, u32 end)`. Every format parameter that would
   otherwise be a branch in the inner loop (bit depth, channel count, sRGB, renormalize) is a
   template parameter. Dispatch happens once, outside the loop, in a `switch` on the runtime
   values.
2. **No allocation, no branches on format in the loop.** Outputs are allocated by the caller. The
   body reads `src` and writes `dst` through `KILN_RESTRICT` pointers; edge handling is hoisted so
   only the last row or column pays for a clamp.
3. **Deterministic.** Only `+ - * /`, `std::sqrt` and `std::floor` on floats, no FMA (the
   translation unit turns contraction off), no reduction whose order depends on the split.
   Integer kernels are order-free by construction. A kernel that accumulates floats across items
   (for example smooth normals) is not splittable and says so in the table.
4. **Marked.** Every kernel carries `KILN_HOT` and appears in the table at the top of
   `kernels.h` with its split unit (rows, items, or none) and its SIMD status (auto, intrinsics, or
   scalar). That table is how hot paths stay visible; there are no scattered "this is hot" comments.
5. **Reference-checked.** When a kernel gains an intrinsics path, the scalar template stays as the
   reference and a test runs both on random inputs and compares bytes.

### Splitting work

- `parallel_for(JobSystem const* jobs, u32 count, u32 grain, fn, user)` in the cook internals runs
  `fn(user, begin, end)` over `[0, count)` in chunks of `grain`. The calling thread executes chunks
  itself and takes remaining chunks through an atomic counter, so it always makes progress: no
  deadlock when `jobs` is null, when the pool has one thread, or when the cook itself runs on a
  runtime worker (cook-on-miss). Workers never allocate inside a chunk.
- The cooker receives the job system through `CookEnv { alloc, diag, jobs }`, which replaces the
  trailing `alloc, diag` parameters of `cook_mesh` and `cook_texture`. `kiln-cook` and the
  cook-on-miss provider pass the pool they already own.
- **Textures** split by row bands: the fused "prepare level 0" pass (convert + flip green +
  renormalize in one kernel) and the level 0 to level 1 downsample. Further levels together are a
  third of that work and stay single-threaded.
- **Meshes** split by (part, LOD): the compute stages of one LOD geometry form one task with its
  own arena; `pack_lod` and the material and layout interning then run sequentially in traversal
  order, so the output is unchanged.

### What stays scalar

PNG decode (wuffs, inherently serial), the KTX2 and `.mesh` writers (memcpy), the mesh cooker's
own arithmetic, and `generate_normals` (order-dependent float accumulation).

## Rollout

| Step | Change | Verifies |
|---|---|---|
| 1 | `CookStats` on `CookedTexture` / `CookedMesh` with per-stage microseconds; `kiln-cook --verbose` prints them; `bench_image` executable (manual, not CTest) cooks synthetic 2K/4K/8K images | numbers before and after every later step |
| 2 | `kernels.h`; `image.cpp` kernels specialized by template; 64 KiB `u16 -> u8` table replaces the binary search; fused prepare-level-0 kernel; hoisted edge clamps | `test_image`, `test_texture_cook`, `test_texture_golden` byte-identical |
| 3 | `parallel_for`, `CookEnv`, row-band split of prepare and first downsample; providers pass the pool | same tests with 1 and N threads |
| 4 | per-(part, LOD) mesh tasks with per-task arenas, sequential merge | `test_mesh_cook`, `test_mesh_golden` byte-identical |
| 5 | intrinsics only for kernels that the benchmark shows did not auto-vectorize, with the reference test | reference test |

Steps 1 and 2 are independent. Step 3 needs 2; step 4 needs 3; step 5 waits for measurements.

Status (2026-09-27): steps 1 to 4 landed. Step 5 landed for the renormalize kernel (SSE2, x64
baseline, so no ISA decision was needed). Measured on a 4096x4096 RGBA8 image, MSVC release,
single thread: `downsample_2x` linear 71 ms to 13 ms, sRGB 190 ms to 21 ms, renormalize 450 ms to
162 ms; with a pool of 8 the row split gives a further 2x to 4x on the split passes. Remaining
candidates: the renormalize step inside the normal-map downsample still uses the scalar routine,
and the 8-bit divides could come from a 256-entry table of exact doubles.

## Rationale

- Template specialization before intrinsics: all three compilers vectorize a branch-free
  fixed-stride loop, and the result is portable with no per-ISA code to maintain.
- The sRGB table replacement is algorithmic and exact, so it needs no determinism argument.
- A helping `parallel_for` avoids a second scheduler and cannot deadlock inside a worker.
- Per-task arenas keep the "all cook memory in the arena" rule while removing the shared-arena
  hazard.

## Alternatives considered

| Alternative | Why not now |
|---|---|
| ISPC or Highway | A dependency for a handful of kernels; revisit if step 5 shows real ISA-specific work |
| Threading the PNG decode | wuffs is serial; the gain would come from decoding straight into the target layout instead |
| SIMD in the mesh cooker | meshoptimizer and MikkTSpace dominate; task parallelism is the lever |
| A general job graph | The two split shapes (rows, items) cover every kernel here |

## Consequences / what this constrains later

- Encoders added in v0.6 (BCn, ASTC, Zstd) enter through the same kernel table and split by
  blocks or rows.
- `CookEnv` is the place for future cook-wide services (progress callback, cancellation).

## Open points for the owner

- Should `bench_image` also run in CI as a smoke build (no timing assertions), or stay manual?
- Baseline ISA for a future intrinsics path: SSE4.1 on x64 and NEON on arm64, or AVX2 on x64?
