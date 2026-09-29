# Cook kernels: hot paths, SIMD and task splitting

**Status:** Proposed (awaiting owner sign-off). The owner asked for the review and approved the
rollout. Implemented: `src/cook/kernels.h`, `src/cook/image.cpp`, `src/cook/parallel.{h,cpp}`,
`src/cook/mesh_cook.cpp`, `CookEnv` in `include/kiln/cook/cook.h`, `JobBudget` in
`include/kiln/cook/image.h`.
**Decides:** The kernel contract for the cooker's hot paths, SIMD rules, row and item splitting,
`CookEnv`, and the rollout order.

## Decision

The cooker's per-texel and per-vertex work is organized as **kernels**: small pure functions with
a fixed shape that the compilers can vectorize, that can be split across worker threads by rows or
by items, and whose output is byte-identical in every configuration. The rest of the cooker
(parsing, planning, interning, file writing) is ordinary code.

### Where the time goes

- **Textures.** The image passes over level 0 (convert, flip green, renormalize, downsample)
  dominate. Generic per-texel loops that branch on bit depth and channel count do not vectorize.
- **Meshes.** The cooker's own per-vertex loops are cheap next to meshoptimizer, MikkTSpace and
  cgltf. SIMD does not help there. Every (part, LOD) pair is independent up to the final interning
  into shared tables, so task parallelism is the lever.

### Kernel contract

A kernel lives in `src/cook/kernels.h` (internal, never installed) and follows these rules:

1. **Fixed shape.** A function over a row range or an item range. Every format parameter that
   would otherwise be a branch in the inner loop (bit depth, channel count, sRGB, renormalize) is a
   template parameter. Dispatch happens once, outside the loop.
2. **No allocation, no branches on format in the loop.** The caller allocates outputs. The body
   reads and writes through `KILN_RESTRICT` pointers; edge handling is hoisted so only the last row
   or column pays for a clamp.
3. **Deterministic.** Only `+ - * /`, `std::sqrt` and `std::floor` on floats, no FMA (the
   translation units turn contraction off), no reduction whose order depends on the split. A kernel
   that accumulates floats across items (for example smooth normals) is not splittable.
4. **Marked.** Every kernel carries `KILN_HOT` and appears in the table at the top of `kernels.h`
   with its split unit and SIMD status (auto, intrinsics, or scalar). There are no scattered "this
   is hot" comments.
5. **Reference-checked.** A kernel with an intrinsics path keeps the scalar template as the
   reference, and a test compares both on random inputs byte for byte (`test_image.cpp`).

### Splitting work

- `parallel_for(jobs, alloc, count, grain, fn, user, maxThreads)` (`src/cook/parallel.h`) runs
  `fn(user, begin, end)` over `[0, count)` in chunks of `grain`. The calling thread runs chunks
  itself and claims the rest through an atomic counter, so it always makes progress: no deadlock
  when `jobs` is null, when the pool has one thread, or when the cook runs on a runtime worker
  (cook-on-miss). Chunks never allocate.
- `cook_mesh` and `cook_texture` take `CookEnv { alloc, diag, jobs, maxThreads }`. `kiln-cook`
  passes its own pool (sized by `--threads`); the cook-on-miss provider passes the context's.
- **Thread budget.** `maxThreads` caps how many threads, the caller included, work on one cook
  (`parallel_for` submits at most `maxThreads - 1` helpers). The default is 3: the measurements
  show most of the gain by then, and a cook must not crowd out the host's render, net or game
  threads that share the pool. `1` runs inline; `0` lifts the cap. The image functions take the
  same pair as `JobBudget { jobs, maxThreads }`.
- **Textures** split by row bands: the fused "prepare level 0" pass (convert + flip green +
  renormalize in one kernel) and the level 0 to level 1 downsample. Further levels together are a
  third of that work and stay single-threaded.
- **Meshes** split by (part, LOD): building (expand, bake, normals, tangents, weld, optimize) and
  quantizing run as parallel rounds, one task per (part, LOD); planning and the merge into shared
  tables run sequentially in traversal order, so the output does not change.
- Worker priority belongs to the pool, not the cooker: `ThreadPoolDesc::priority`
  (`threading-and-io.md`).

### What stays scalar

image decode (wuffs, inherently serial), the KTX2 and `.mesh` writers (memcpy), the mesh cooker's
own arithmetic, and `generate_normals` (order-dependent float accumulation).

## Rollout

| Step | Change | Verifies |
|---|---|---|
| 1 | `CookStats` on `CookedTexture` / `CookedMesh` with per-stage microseconds; `kiln-cook --verbose` prints them; `kiln_bench_image` (manual, not CTest) cooks synthetic 2K/4K/8K images | numbers before and after every later step |
| 2 | `kernels.h`; image kernels specialized by template; a 64 KiB `u16 -> u8` table replaces the sRGB binary search; fused prepare-level-0 kernel; hoisted edge clamps | `test_image`, `test_texture_cook`, `test_texture_golden` byte-identical |
| 3 | `parallel_for`, `CookEnv`, row-band split of prepare and first downsample; providers pass the pool | same tests with 1 and N threads |
| 4 | per-(part, LOD) mesh tasks with per-task arenas, sequential merge | `test_mesh_cook`, `test_mesh_golden` byte-identical |
| 5 | intrinsics only for kernels the benchmark shows did not auto-vectorize, with the reference test | reference test |

Steps 1 to 4 landed on 2026-09-27. Step 5 landed for the renormalize kernels (SSE2, the x64
baseline, so no ISA decision was needed). Measured on a 4096x4096 RGBA8 image, MSVC release,
single thread: `downsample_2x` linear 71 ms to 13 ms, sRGB 190 ms to 21 ms, renormalize 450 ms to
162 ms, then 133 ms with the 8-bit table; the normal-map downsample 114 ms to 33 ms on the shared
SSE2 routine. With a pool the row split gives a further 2x to 4x on the split passes, and the
default budget of 3 threads captures most of it.

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
| Threading the image decode | wuffs is serial; the gain would come from decoding straight into the target layout instead |
| SIMD in the mesh cooker | meshoptimizer and MikkTSpace dominate; task parallelism is the lever |
| A general job graph | The two split shapes (rows, items) cover every kernel here |

## Consequences / what this constrains later

- The BC encoders split by block rows through `parallel_for` (`bc_encode.cpp`); Zstd compresses
  one texture's levels on one thread. ASTC (v0.9) will enter the same way.
- `CookEnv` is the place for future cook-wide services (progress callback, cancellation).

## Open points for the owner

- Worker priority: decided and implemented in the pool (`ThreadPoolDesc::priority`).
- Should `kiln_bench_image` also run in CI as a smoke test (no timing assertions)? It is compiled
  in every CI build that has `kiln_cook`, but not run.
- Baseline ISA for a future intrinsics path: SSE4.1 on x64 and NEON on arm64, or AVX2 on x64?
