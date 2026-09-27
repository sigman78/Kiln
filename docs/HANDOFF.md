# Hand-off: Graphics-API-Agnostic Asset Library (working name `kiln`)

**Audience:** coding agent starting the project.
**Status:** v0.2 hand-off. Direction is fixed; exact dependencies, container set and API shape are to be settled with the project owner before and during M0. **Ask before locking in anything marked _(discuss)_.**
**Companion doc:** `mesh-format-spec.md` (draft cooked mesh format, to be adopted as kiln's runtime mesh format).
**First target:** **v0.5**, a usable async asset loader that proves the concept and API in real apps (§11). Advanced features and niche optimizations come after, and the design must leave room for them without building them now.

---

## 1. What we are building

A small, open-source, modern C++ library that sits between **source assets** and **any renderer**:

```
source (glb, png, ktx2) ──► cook (in-process or CLI) ──► cooked store (content-hashed)
                                     │                            │
                                     └─(cache-less: in memory)────┤
                                                                  ▼
renderer (Vulkan / sokol / bgfx / custom) ◄── adapter ◄── async runtime loader (+ hot reload)
```

It loads glTF 2.0 (`.glb`, optionally `.gltf`), KTX2 and PNG sources. It cooks them into GPU-ready, API-agnostic runtime formats: the `.mesh` spec and KTX2. Those are loaded asynchronously, decoded directly into memory the renderer provides, and optionally hot-reloaded on source change.

### 1.1 Core rules

1. **The library never calls a graphics API.** All GPU interaction goes through a small **adapter interface** the renderer implements (§6.4). This boundary is the central design constraint; don't erode it for convenience.
2. **The runtime consumes only cooked formats, or data the host hands it directly.** It never reads raw source files. In dev builds this is transparent: requesting an asset whose cooked form is missing triggers cook-on-miss behind the handle. Procedural and runtime-generated data enters through **in-memory registration** (§6.3), with the same handles, states, events and upload path.
3. **Cooking is a host-side activity.** Shipping builds, and any device other than the dev host (mobile, consoles), contain only cooked data for their target and link only `kiln_core` + `kiln_runtime`. The cooker is a **cross-cooker**: it runs on the dev machine or CI and produces output for a target profile (§5.3).

### 1.2 Goals

- One implementation of cooking, shared by the CLI (`kiln-cook`, for build/CI) and dev builds of the host app (cook-on-miss).
- Async, prioritized loading with explicit, bounded memory use.
- Optional hot reload, via file watcher, generation-counted handles, and change events.
- Declarative, layered cook settings: intent separated from per-target encoding (§5).
- Fast compile times, small binaries, exception-free code, minimal and controlled allocation.
- Windows first, then Linux, then macOS.
- *Post-v0.5:* progressive / partial loads (texture mip ranges and mesh LODs, smallest first), block compression, cross-cooking for other targets.

### 1.3 Non-goals (v1)

- Rendering, shaders, pipelines, materials-as-shaders. The host renderer owns these.
- Scene graph, ECS, gameplay. We output asset data plus metadata (parts, mounts, material names).
- Animation, skinning, morph targets (reserve format space only).
- Importing FBX, OBJ, LWO, USD. Those are converted to glb by *external* tools.
- Sparse or virtual texturing, GPU decompression, DirectStorage. Keep the door open (chunked compression, range reads), but don't implement them.
- Remote asset servers or network VFS (possible later for on-device iteration, via the `IoBackend` abstraction).

---

## 2. Engineering principles (owner preferences)

**Language level: C++20.** C++23 features only when all three target compilers support them (MSVC is the constraint). "Orthodox C++" flavor with compact, useful templates allowed.

**Exceptions and RTTI: don't use them, but don't disable them yet.**
- Default builds keep the normal compiler settings. Library code must not `throw`, `try`/`catch`, `dynamic_cast` or `typeid`; code review enforces this.
- Third-party dependencies may use exceptions internally for now. Catch at our boundary only if a dependency throws on bad input, and convert to `Status`.
- The v1.0 **hardening** step adds `KILN_NO_EXCEPTIONS` / `KILN_NO_RTTI` options (`/EHs-c- /GR- /D_HAS_EXCEPTIONS=0`, `-fno-exceptions -fno-rtti`) and makes CI build with them. Writing code that would compile under those flags today keeps that step cheap.

**Declarative style for state, parameters and large arguments.** Prefer aggregate description structs with default member initializers, passed with C++20 designated initializers, over long parameter lists, builder objects or setter sequences:

```cpp
auto r = kiln::cook_mesh(src, {
    .profile   = kiln::VertexProfile::Default,
    .genLods   = false,
    .posTolMm  = 0.1f,
}, diag);
```

- Structs are POD-like, cheap to copy or pass by `const&`, and every field has a sensible default, so callers set only what they mean.
- The same applies to context creation, request options, cook settings and adapter descriptors.

**Header hygiene: a recommendation, with one hard rule.**
- **Hard rule:** no `<iostream>`, `<sstream>`, `<fstream>` or `<iomanip>`, anywhere.
- **Recommended:** public headers stick to lightweight headers (`<cstdint> <cstddef> <cstring> <new> <type_traits> <bit> <atomic> <utility> <concepts>`). Heavier std headers (`<string> <vector> <functional> <memory> <algorithm> <optional> <variant> <chrono> <thread> <mutex> <filesystem>` …) belong in `.cpp` files. If a public header genuinely benefits from one, it's acceptable; note why in the PR.
- Third-party headers (fastgltf, libktx, …) should stay in `.cpp` files behind our own types.

**Small vocabulary types** live in `kiln/core` for now:
- `Span<T>`, `StrView`
- `FixedArray<T,N>`, `Vec<T>` (allocator-aware, trivially-relocatable fast path)
- `HashMap<K,V>` (open addressing)
- `Result<T>` / `Status`
- `Handle<T>`
- `FunctionRef` (non-owning callable)

In principle these belong in a separate utility library, which isn't being built now. Keep `kiln/core` **self-contained**, with no dependencies on any other kiln module, so it can be lifted out later without churn. Keep the set minimal: add a type only when two or more modules need it. Owning callables should be avoided; use function pointer + `void* user`.

**constexpr-everything** where it's meaningful:
- hashing (FNV-1a 64 / xxh-style)
- fourcc
- format tables (bytes per block, block extents, sRGB pairs)
- layout validation
- enum ↔ string tables
- `static_assert` on every on-disk struct size and offset

**Allocation discipline.**
- All allocation goes through a user-supplied `Allocator` interface: alloc, free, and a tag for accounting. There are no hidden globals except the default allocator.
- Use arenas / linear allocators for per-cook and per-load temporaries, reset in bulk.
- There should be no allocation on the steady-state hot path. Polling handles, pumping completed loads, and looking up assets by ID must not allocate.
- Every allocation is tagged (`Tag::Io`, `Tag::Cook`, `Tag::Registry`, …) with stats queryable at runtime.

**Error handling.**

*Recoverable errors return values:*
- `Status` is a compact enum code plus an optional detail string in a caller-provided diagnostic sink.
- `Result<T>` is a value-or-`Status`. There's no heap in the error path.
- Recoverable errors include IO failures, parse errors, validation failures, and unsupported formats. Their contract is: the asset goes to the `Failed` state, a placeholder is served, and a diagnostic is emitted.

*Non-recoverable errors panic:*
- `KILN_PANIC(msg)` and `KILN_VERIFY(cond)` are always on; `KILN_ASSERT(cond)` is debug only. Use them for broken invariants, API misuse, and out-of-memory from the allocator.
- The panic handler is user-overridable. The default logs to the log sink and aborts.

**Logging:** user-supplied log sink callback (level, category, message), formatted with a tiny internal `snprintf`-style formatter. No iostreams.

**Threading:**
- The library can run on a user-provided job interface (submit, plus an optional wait), with a small built-in thread pool as the default.
- Completion is delivered only through `pump()` on the thread the host chooses. No user callbacks fire from worker threads.
- Lock-free or short-critical-section queues. Implementation files may use `<thread>`/`<mutex>` or a thin OS wrapper *(discuss)*.

**OS-dependent code:** simplest portable implementation first (C/POSIX file IO, polling file watcher). Native implementations come after the API has taken shape, behind the same interfaces.

**Style:**
- `.clang-format` in repo.
- Warnings as errors: `/W4`, `-Wall -Wextra -Wpedantic` plus a curated set.
- No `using namespace` in headers.
- Everything lives in `namespace kiln`.

**Compile-time budget:** track full-rebuild time of the library in CI and flag regressions greater than 20%.

---

## 3. Repository layout (proposed)

```
kiln/
  CMakeLists.txt  CMakePresets.json  LICENSE  README.md  HANDOFF.md  CHANGELOG.md
  docs/            mesh-format-spec.md, cook-settings.md, design notes, open-questions.md, api-friction.md
  include/kiln/    core.h  alloc.h  result.h  containers.h  hash.h  log.h
                   io.h  formats.h  mesh.h  texture.h  settings.h  cook.h  assets.h  adapter.h
  src/core/        allocators, panic, log, containers impl, OS wrappers
  src/io/          compat file backend, read queue, polling file watcher
  src/formats/     .mesh read/write, KTX2 read (+write in cook), png decode
  src/cook/        glb import → .mesh; image → KTX2; settings resolution; validation; store writer
  src/runtime/     asset registry, handles, request scheduler, sources, hot reload
  tools/           kiln-cook (CLI), kiln-info (dump .mesh/.ktx2)
  examples/        null_adapter (headless), viewer (Vulkan 1.4 example adapter), sokol/bgfx (later)
  tests/           unit, golden files, fuzz corpus (later)
  third_party/     vendored or FetchContent-pinned deps (see §8)
```

**Split into CMake targets:**

| Target | Contents | Linked by |
|---|---|---|
| `kiln_core` | core types, allocators, logging | everything |
| `kiln_runtime` | runtime loading, `.mesh` + KTX2 readers, IO | all builds, incl. shipping |
| `kiln_cook` | glTF / PNG / encoders / settings / validation / store writer | dev builds and tools only |

`kiln_runtime` must build and link **without** `kiln_cook` or its dependencies. Cook-on-miss works through a **cook provider** that `kiln_cook` registers with the runtime at startup, so the runtime has no hard dependency on it.

---

## 4. Data formats

### 4.1 Sources

- **Mesh:** glTF 2.0 binary (`.glb`); `.gltf` + external files optional.
  - Handle: nodes with names and hierarchy, meshes and primitives, materials (name, alpha mode, texture bindings, UV set), samplers (carried as hints), `extras` on nodes and materials, empties with the `mount_` prefix, and the `_lodN` / `col_` / `_` conventions.
  - **`EXT_meshopt_compression` input: planned, not in v1.** Structure the importer's buffer-view resolution so a decode step can be inserted later; meshoptimizer is already a cook dependency and includes the decoder. Until then, reject with a clear diagnostic.
  - Reject with a clear diagnostic: Draco (`KHR_draco_mesh_compression`), sparse accessors *(discuss)*.
- **Image:** PNG (8/16-bit), KTX2 (pass-through when already runtime-ready for the target, otherwise re-encode). EXR later *(discuss)*.

### 4.2 Cooked runtime formats

- **Mesh:** the `.mesh` format per `docs/mesh-format-spec.md` (v0.3 draft; **unstable until v1.0**, and the cooker version in the store key handles invalidation).
  - Readers are zero-copy views over the loaded metadata blob.
  - The writer is deterministic, so identical input and settings give byte-identical output.
  - LOD records and chunk layout must stay compatible with later progressive loading (smallest LOD blob readable independently).
  - **Payload compression is designed in, not implemented.** The GPU payload is described by a blob table (`BLOB`, spec §5.9) mapping encoded file ranges to decoded payload ranges, each with a codec (None / Zstd / meshopt vertex and index) and a filter (byte shuffle, delta, meshopt filters). Draw records only use decoded offsets, so compression never touches draw-facing data.
  - **v0.5:** the cooker writes codec `None` only, with the `kPayloadRaw` fast path (one direct read into staging). The runtime implements the full blob decode *loop* (per-blob read and decode into the destination, with gap zero-fill and validation) and supports `None`. Adding a codec later is then just a new decoder.
  - The default scheme (Zstd + byte shuffle vs meshopt vs both) is picked later **by measurement** on real assets.
- **Texture:** KTX2.
  - v0.5: raw formats (RGBA8, RG8, R8, R16, RGBA16F) with gamma-correct mips (plus normal-map renormalization).
  - Later: BCn (BC7 color, BC5 normals, BC4 single channel, BC6H HDR), ASTC/ETC2 for mobile targets, Zstd supercompression, alpha-coverage-preserving mips.
  - **Mip levels indexed so the smallest mips can be read first** (range reads, post-v0.5).

### 4.3 Where cook results go

Cooking is a pure function, `cook(source, resolved settings) → blob in memory`. The result then goes to one of three places:

```
cook(...) → blob ─┬─► store      write to disk, reuse later (normal dev + CLI)
                  ├─► runtime    load directly from memory, discard after (cache-less mode)
                  └─► nothing    validate only, report diagnostics (kiln-cook --check)
```

- **Cache-less mode** is for viewers and tools (drag-and-drop preview), tests (no filesystem state), small demos with a fast profile, read-only or sandboxed environments, and desktop mod loading if the host ships `kiln_cook`. It re-cooks on every load, so it's practical only with cheap settings and modest asset counts.
- **Validate-only** backs `kiln-cook --check` and pre-commit / CI checks.

### 4.4 Cooked store (a.k.a. cache)

**What it is:** the directory where cooked outputs live. Both `kiln-cook` and cook-on-miss dev builds write to it, and the runtime reads from it. For shipping, the store for one target (optionally packed) *is* the game data.

**Why it exists:** cooking is expensive and outputs are pure functions of their inputs, so the store makes cooking **incremental**.
- Tangents, mesh optimization and mip generation take milliseconds to seconds per asset. BC7 encoding of a 4K texture can take seconds even multithreaded.
- The CLI and cook-on-miss dev builds share results: whichever cooked an asset first, the other reuses it.
- Hot reload re-cooks only the changed asset and its dependents.
- Switching git branches doesn't re-cook, because both versions stay under different content hashes.
- CI can persist the store between runs.

**Design:**
- **Key:** `hash(source bytes, resolved settings, target profile, cooker version)`. Filenames are the content hash, so invalidation is simply "key not found" and nothing is ever overwritten. Several targets can coexist in one store.
- **Writes are atomic** (temp file, then rename). A corrupt entry is a recoverable error: re-cook it.
- *v0.5:* no index file. The key is computed by hashing the source on request, which is fine at demo scale.
- *Later:* an index file (asset ID → key, source path, dependencies, timestamps/sizes for fast "unchanged" checks), and garbage collection via `kiln-cook --gc` (never automatic at runtime).

**The store must stay optional in the runtime API.** A host that cooks everything in its build step can treat it as a plain read-only directory.

---

## 5. Cook settings and resolution

Detailed design goes into `docs/cook-settings.md`. This section fixes the model.

### 5.1 Layered resolution

The cooker resolves every texture and mesh into one concrete settings struct (`TextureCookSettings` / `MeshCookSettings`). Layers, from weakest to strongest:

```
1. built-in defaults
2. inferred usage       (glTF material slot → "color", "normal", "orm", …)
3. project presets      (named intents: color, color_masked, normal, mask, hdr, ui, lut, height …)
4. target encodings     (per target, per preset: desktop color → BC7, mobile color → ASTC 6x6 …)
5. path rules           (glob → preset/overrides: "ui/**" → ui)
6. per-asset sidecar    (hull_albedo.png.kiln / ship_hauler_a.glb.kiln)
7. session overrides    (dev fast profile, CLI flags, host-supplied structs)
        ↓
resolved settings struct → hashed into the store key
```

**Principles:**
- **Separate intent from encoding.** A preset says what a texture *is* (sRGB color with cutout alpha, tangent-space normal, packed ORM mask). A target says how that kind of texture is encoded on a platform. Adding a platform touches only target tables, never assets.
- **Inference covers most cases.** The glTF material slot already implies usage: baseColor → color (sRGB), normalTexture → normal, metallicRoughness → ORM (linear). Rules and sidecars are for exceptions.
- **Only resolved values are hashed.** Editing the mobile target doesn't invalidate desktop outputs.
- **The C++ structs are the real interface.** Config files are a thin layer parsed only by `kiln_cook`; a host can skip them and pass structs directly.
- **Unknown keys and invalid combinations are cook errors**, e.g. premultiplied alpha with no alpha, or BC5 on a color texture.
- **`kiln-cook --explain <asset> --target <t>`** (post-v0.5) prints which layer set each field.
- **Per-part mesh overrides** may come from glTF `extras` on nodes (e.g. `lod_ratio`, `no_simplify`), which artists can set in their DCC tool.

### 5.2 Setting groups (full model; v0.5 implements a subset)

**Texture:**

| Group | Settings |
|---|---|
| Semantics | color space; channel packing (e.g. ORM from separate images); swizzle; normal-map handling (renormalize, green-flip, 2-channel storage) |
| Alpha | none / mask (cutoff, coverage-preserving mips) / blend; premultiplied or straight; UV-island dilation |
| Encoding | format per target (BC1/4/5/6H/7, ASTC block size, ETC2, raw); quality/effort; RDO lambda |
| Supercompression | none / Zstd level / Basis (ETC1S/UASTC) |
| Mips | full / none / max levels / min level size; filter kernel; wrap hint for edge filtering |
| Size | max size per target, downscale bias, power-of-two policy |
| Streaming | resident-minimum mip count; priority class |
| Shape | 2D / array / cube (6 files or equirect) / 3D |
| Sampler hints | wrap and filter from glTF samplers, carried as metadata; the renderer may ignore them |

**Mesh:**

| Group | Settings |
|---|---|
| Encoding | vertex profile (default/precise), quantization tolerances, index width policy |
| Processing | tangent generation, vertex cache/overdraw/fetch optimization, weld tolerance |
| LOD | generate yes/no; count or target ratios; error thresholds; attribute weights; lock borders; or "authored `_lodN` only" |
| Compression | scheme: none / basic (Zstd + byte shuffle) / meshopt / meshopt + Zstd; Zstd level; blob chunk size. Selectable per target and per asset. Post-v0.5 |
| Import | unit/axis override, prefixes to strip, merge parts |

### 5.3 Targets and cross-cooking

- A **target profile** (e.g. `desktop`, `mobile`) holds the per-preset encoding tables plus caps (max texture size, vertex profile).
- The target is part of the store key.
- A shipping package is the cooked store for one target. On-device iteration uses host cooking plus copying files over (later: a network `IoBackend`).
- *v0.5:* a single implicit target, `desktop` with raw formats.

### 5.4 v0.5 subset

- Built-in defaults, inference from glTF material slots, and overrides through the C++ settings structs only.
- Texture settings: color space, normal renormalization, mips on/off, max size.
- Mesh settings: vertex profile, tangent generation, optimization on/off, use authored LODs.
- No config files, presets, rules, sidecars or `--explain` yet. Keep the struct and resolution code shaped so they slot in without changing the structs' meaning.

---

## 6. Architecture

### 6.1 IO layer

- `IoBackend` interface: open, size, async ranged read into caller memory, close.
- **v0.5: a single "compatibility" backend:** plain C stdio / POSIX file IO (`fopen`/`fread`/`fseek`, or `open`/`pread` where available), with blocking reads run on worker threads. It's the same code on all OSes and good enough to shape and test the API.
- **True async backends come later:** Win32 overlapped IO / IoRing, io_uring, dispatch IO on macOS. They must drop in behind the same interface; if the compatibility interface can't support them cleanly, fix the interface early.
- Mountable roots (directories now; pack files later *(discuss)*).
- Reads are **range-based** and target **caller-provided memory**, which enables direct-to-staging loads.

### 6.2 Cook layer (`kiln_cook`)

- `cook_mesh(source, settings, alloc, diag) → Result<CookedBlob>` and `cook_texture(...)`.
- **Pure functions:** no global state, thread-safe, arena-backed temporaries.
- Settings resolution (§5) happens before cooking; the cook functions receive resolved structs only.
- Validation produces structured diagnostics (code, severity, asset, node or material, message). Codes are stable so they can be documented for artists.
- Mesh pipeline: import → coordinate/units normalization → split per material → MikkTSpace tangents → optimize → quantize → build LOD records → write.

### 6.3 Runtime layer (`kiln_runtime`)

- **Registry:**
  - Assets are identified by `AssetId` (64-bit path hash).
  - `Handle<T>` = {index, generation}.
  - States: `Unloaded → Pending → MetaReady → Ready`, or `Failed`, which serves a placeholder. A `Partial` state for progressive loads comes post-v0.5.
  - **`MetaReady` (meshes):** the CPU metadata (parts, mounts, bounds, material names) is loaded and `view()` works, but the GPU payload is still in flight. It arrives with the first small read, so gameplay can place entities, attach to mounts and cull by bounds before geometry is drawable. Textures can go straight from `Pending` to `Ready` *(discuss: whether textures also expose MetaReady for extent/format)*.
- **Placeholders:** every texture handle is usable from the moment it's requested.
  - Placeholders are chosen by **texture kind** (inferred from the glTF material slot) so a partially loaded scene still looks plausibly lit:

    | Kind | Placeholder |
    |---|---|
    | Base color | mid-grey |
    | Normal map | flat normal (0.5, 0.5, 1) |
    | ORM | AO 1, roughness 1, metallic 0 |
    | Emissive | black |
    | Failed (dev builds) | loud magenta checker |

  - Placeholders are created once through the adapter at context creation and are host-overridable per kind.
  - Meshes have no geometry placeholder. The host skips the draw until `Ready`; a dev-only bounding-box proxy is optional, and coarsest-LOD-first comes with progressive loading.
- **Load groups:**
  - `Group g = kiln::group(ctx)`, then `request(..., { .group = g })` to add requests. A request may belong to one group.
  - `progress(ctx, g) → GroupStatus { ready, failed, pending, bytesDone, bytesTotal }`, which drives loading screens.
  - `wait(ctx, g, { .timeoutMs })` blocks until every member is `Ready` or `Failed`, or the timeout expires, by looping `pump` plus a short sleep. It returns the same `GroupStatus`.
    - A group is "settled" when every member is `Ready` or `Failed`. Failures aren't fatal to `wait`; the caller decides.
    - On timeout, `wait` returns partial results.
    - Waiting raises the group's members to high priority.
  - `wait` must be called on the thread that calls `pump()`. It requires a self-submitting adapter (§6.4). Violating either rule is a panic with a clear message, never a hang.
  - Recommended usage: block with `wait` only on tiny critical groups (fonts, loading-screen art). For level loads, keep the frame loop running, calling `pump()` and showing `progress`, then stream everything else behind placeholders.
- **Sources:** the loader reads through an abstract source, either a **file range** or a **memory span**. Memory sources serve cache-less cooking and in-memory registration.
- **In-memory registration:** `register_mesh(ctx, desc, data)` / `register_texture(...)` let the host hand in already-decoded data (procedural or generated content, tests, mods). It returns a normal handle with the same states, events and adapter upload path.
- **Requests:**
  - `request(ctx, id, options)`. Options include priority and a **range field, reserved** until partial loads land.
  - Requests are refcounted and released explicitly.
- **Scheduler:** budgets for in-flight IO bytes, decode jobs, and upload bytes per `pump()`, with back-pressure from the adapter.
- **Pipeline per load:**
  1. read metadata
  2. ask the adapter for destination memory
  3. read and decode into it
  4. adapter `commit`
  5. adapter reports GPU completion
  6. state becomes Ready on `pump()`
- **Cook-on-miss (dev):** if the store misses and a cook provider is registered, cook on a worker, write to the store (or keep in memory in cache-less mode), then continue.
- **Hot reload:**
  - A file watcher marks sources dirty; dependencies propagate (v0.5: glb → its textures only).
  - Re-cook, then load the new version behind the same handle and bump the generation.
  - Emit a `Changed` event on `pump()`.
  - The old payload is released via the adapter's **deferred** destroy (frames in flight are the renderer's responsibility).

### 6.4 Adapter interface (the renderer boundary)

Sketch only; the shape is *(discuss)*.

```cpp
struct UploadDesc {               // what the library wants to place
    AssetId  id;
    Kind     kind;                // MeshPayload, TextureLevels, ...
    uint64_t size;
    uint32_t alignment;
    TextureDesc const* texture;   // format, extent, levels, range
};

struct UploadTarget {             // where the renderer wants it
    void*    dst;                 // mapped staging / ReBAR / host-image-copy scratch
    uint64_t rowPitchAlign;       // copy constraints the library must honor
    uint64_t token;               // renderer's opaque ticket
};

struct Adapter {
    // capability negotiation
    bool   (*supports_format)(void* user, Format f, FormatUsage u);
    void   (*copy_constraints)(void* user, CopyConstraints* out);
    // placement (may return "not now" → back-pressure)
    Status (*begin_upload)(void* user, UploadDesc const&, UploadTarget* out);
    void   (*commit_upload)(void* user, uint64_t token);        // data written, submit
    bool   (*is_upload_complete)(void* user, uint64_t token);   // polled during pump()
    // visibility: called during pump() when an asset becomes Ready or is hot-reloaded
    void   (*publish)(void* user, AssetId id, GpuObject obj, uint32_t generation);
    // lifetime
    void   (*destroy_deferred)(void* user, GpuObject obj);      // renderer delays by frames in flight
    // optional residency hooks (budget, eviction requests) — later
    uint32_t caps;                                              // AdapterCaps, e.g. kSelfSubmitting
    void*  user;
};
```

**Two binding models:** the library supports both, and the renderer picks one.

- **Per-frame lookup.** `kiln::gpu(ctx, handle)` returns the current `GpuObject`: the placeholder while Pending or Failed, the real object once Ready, the new version after a hot reload. It's a table lookup, allocation-free. This works with any renderer (sokol/bgfx-style binding) and needs no `publish` logic.
- **Stable bindless slot (recommended for Vulkan 1.4).**
  - At request time the adapter allocates a descriptor-array slot and writes the placeholder into it. Materials store the slot index, which never changes.
  - On `publish`, the adapter overwrites the same slot with the real image, and the old object goes to `destroy_deferred`.
  - Arrival, hot reload and (later) progressive mips then need no per-frame work and no material rebuilds.
  - The slot is exposed through `kiln::gpu(ctx, handle)` as well, so both models share one query.

**`kSelfSubmitting` capability:** the adapter's `commit_upload` submits to a queue by itself (e.g. a dedicated transfer queue), and `is_upload_complete` polls a fence or timeline semaphore. Uploads then make progress without the renderer recording a frame, which `wait()` requires. An adapter that only submits uploads inside the frame's command buffers must not set the flag; `wait()` then panics, and hosts use the keep-rendering pattern instead.

**Frame integration:**

```
frame:
  kiln::pump(ctx, { .uploadBytes = budget })   // completes uploads, calls publish, emits events
  for (auto& e : kiln::events(ctx)) { ... }    // optional: new vertex layout → pipeline, log failures
  record draws:
     textures → bindless slot or kiln::gpu(ctx, h) (always valid)
     meshes   → draw if kiln::is_ready(ctx, m); otherwise skip or draw a dev proxy
```

- Formats are an engine-neutral enum with a direct mapping to `VkFormat` *(discuss: numeric mirror vs compact enum + table)*. Mapping tables for sokol, bgfx and D3D12 come later.
- Ship a **null adapter** (CPU memory only, self-submitting: uploads complete immediately) for tests and tools, and an **example Vulkan 1.4 adapter** used by the example viewer (§11.2). The example adapter is self-submitting (dedicated transfer queue plus timeline semaphore) and uses bindless slots.

---

## 7. Public API sketch (illustrative, not final)

```cpp
kiln::Context* ctx = kiln::create({
    .alloc = &myAlloc, .log = &myLog, .jobs = nullptr /*built-in*/,
    .io = nullptr /*compat backend*/, .adapter = &vkAdapter,
    .storeDir = "cooked/", .sourceRoots = {"assets/"}, .hotReload = true,
});
kiln::cook::install_provider(ctx, { .storeMode = kiln::StoreMode::Disk });   // dev builds only

// critical boot assets: block briefly
kiln::Group boot = kiln::group(ctx);
auto font = kiln::request<kiln::Texture>(ctx, "ui/font", { .group = boot });
kiln::GroupStatus bs = kiln::wait(ctx, boot, { .timeoutMs = 5000 });
if (bs.failed || bs.pending) { /* host decides: abort, retry, continue */ }

// everything else streams in behind placeholders
auto mesh = kiln::request<kiln::Mesh>(ctx, "meshes/ship_hauler_a", { .priority = kiln::Priority::High });

// per frame, on render thread:
kiln::pump(ctx, { .uploadBytes = 64 << 20 });
if (kiln::has_meta(ctx, mesh)) {                // MetaReady or Ready
    kiln::MeshView v = kiln::view(ctx, mesh);   // parts, lods, submeshes, mounts, material names
    // place entities, attach to mounts, cull by bounds...
}
if (kiln::is_ready(ctx, mesh)) { /* draw; kiln::gpu(ctx, mesh) gives the payload GpuObject */ }
for (kiln::Event const& e : kiln::events(ctx)) { /* MetaReady / Ready / Changed / Failed */ }

kiln::release(ctx, mesh);
kiln::destroy(ctx);
```

- Free functions over a context, POD descriptors, designated initializers.
- No virtual interfaces in the public API unless clearly justified *(discuss)*.
- Pre-1.0, API breaks are allowed but must be recorded in `CHANGELOG.md` with migration notes, because an external project will track the library (§11.2).

---

## 8. Candidate dependencies _(discuss — confirm each before adding)_

| Concern | Candidate | Scope | Notes |
|---|---|---|---|
| glTF parsing | **fastgltf** (+ simdjson) or **cgltf** | cook | fastgltf is fastest; check exception/std usage. cgltf is C99, trivial to isolate |
| KTX2 | own minimal reader; **libktx** for writing | runtime read / cook write | Runtime reader is simple enough to write ourselves |
| Zstd | **zstd** | later: runtime decode, cook encode | Decoder-only build for runtime; shared by KTX2 supercompression and `.mesh` blobs |
| PNG | **wuffs**, spng, or stb_image | cook | wuffs is fastest/safest |
| Mesh processing | **meshoptimizer**, **mikktspace** | cook (meshoptimizer's decoder also at runtime, later) | Both small. meshopt codecs, if chosen, need only the decoder sources in `kiln_runtime` |
| BCn / ASTC encode | bc7enc_rdo / bc7e + rgbcx, astcenc, or Compressonator SDK | cook, post-v0.5 | |
| Config parsing | TOML parser, reuse of glTF's JSON parser, or own INI-like | cook, post-v0.5 | *(discuss)* |
| File watch | own polling (v0.5); later native or efsw / dmon | runtime (dev) | |
| Tests | own minimal runner or doctest | tests | Keep light |

**Dependency rules:**
- Pin versions (FetchContent with a commit hash, or vendored).
- Build deps with our flags where possible.
- License must be MIT, BSD, zlib, Apache-2, or similarly permissive.
- Record each dependency in `third_party/README.md` with its license.

---

## 9. Build & platforms

- **CMake ≥ 3.25**, with presets for:
  - `win-msvc-debug/release`
  - `win-clangcl`
  - `linux-clang`, `linux-gcc`
  - `mac-appleclang` (post-v0.5)
- **Options:**
  - `KILN_BUILD_COOK` (ON)
  - `KILN_BUILD_TOOLS` (ON)
  - `KILN_HOT_RELOAD` (ON in dev)
  - `KILN_BUILD_TESTS`
  - `KILN_BUILD_EXAMPLES`
  - `KILN_BUILD_VIEWER` (requires Vulkan SDK)
- **Installable package:** `find_package(kiln)` plus `add_subdirectory` friendly. The external battle-test project will consume it this way from day one.
- **CI (GitHub Actions):** Windows (MSVC, clang-cl) primary; Linux build + tests from M0 to keep the code portable; macOS post-v0.5. Runs build, tests, golden-file checks and the compile-time report.

---

## 10. Testing

- **Unit tests:** containers, hashing, allocators, Result/Status, format tables (mostly `static_assert`), settings resolution.
- **Golden files:** cook sample glbs, then compare `.mesh` / KTX2 byte-for-byte against committed goldens. This also enforces determinism.
- **Round trip:** cook → load via null adapter → validate views (bounds, indices in range, mount transforms). Run in both store mode and cache-less mode.
- **Samples:** a small curated set from the Khronos glTF-Sample-Assets (check licenses per model), plus hand-made edge cases: no UVs, multiple UV sets, mounts, authored LODs, `.NNN` material names, negative scale.
- **Later:** fuzzing (`.mesh` reader, KTX2 reader, glb import); benchmarks (load throughput, cook time).

---

## 11. Plan

### 11.1 v0.5 goal

**v0.5 is a usable async asset loader in real apps.** It proves the concept and validates the decisions that are expensive to change later:

- handle, generation and state semantics
- the adapter interface, against a real Vulkan renderer
- the threading model (workers + `pump()`)
- the error, diagnostic and placeholder flow
- ergonomics of `.mesh` views and cook-settings structs
- the render-while-loading model: placeholders, `publish` into stable slots, `MetaReady`, load groups and `wait`

Quality, performance and platform-reach features come after v0.5, behind interfaces v0.5 establishes.

### 11.2 Two validation tracks (in parallel)

1. **In-repo example viewer.** A small Vulkan 1.4 app in `examples/viewer` implementing the example adapter: staging ring, timeline semaphore, deferred destroy, host image copy optional. It may be built on a thin NoGraphicsAPI-style layer rather than raw Vulkan *(discuss)*. It shows the adapter is implementable in a few hundred lines and demonstrates hot reload.
2. **External battle test (owner-driven).** The owner integrates kiln into a larger project in the Orbital class (possibly Orbital itself) in parallel with development.
   - Keep the library consumable as an external dependency at every milestone: `find_package` / `add_subdirectory`, no assumptions about the host's directory layout.
   - Log friction the owner reports in `docs/api-friction.md`, and review it at each milestone.
   - Record API changes in `CHANGELOG.md` with migration notes.

### 11.3 v0.5 milestones

| M | Deliverable | Done when |
|---|---|---|
| **M0** | Skeleton, CMake + presets, CI (Windows + Linux build), core types (alloc, Result/Status, panic, log, minimal containers, hash, fourcc), `.clang-format`, test runner. **Design notes** for deps, error model, adapter, handles/states and settings structs | Core tests pass; owner signed off on design notes |
| **M1** | `.mesh` read/write per spec (incl. `BLOB` table; codec `None` only, blob decode loop in place); KTX2 read/write for raw formats; `kiln-info` (prints blob table) | Hand-built files round-trip, both `kPayloadRaw` and a multi-blob `None` layout; all spec static_asserts in place |
| **M2** | Cooker: glb → `.mesh` (parts, hierarchy, submeshes, material names, texture bindings + UV sets, mounts, tangents, vertex-cache optimization, default quantized profile with float fallback, authored `_lodN` pass-through if cheap); PNG → KTX2 (RGBA8/RG8/R8, sRGB/linear, gamma-correct mips, normal renorm); KTX2 pass-through; v0.5 settings subset with inference; diagnostics; `kiln-cook <dir>` and `--check` | Golden tests pass; deterministic output; diagnostic codes documented |
| **M3** | Runtime: context, handles, states incl. `MetaReady`, refcounted requests (2 priority levels + boost, reserved range field), `pump()` with upload budget, events, `publish` hook, `gpu()` lookup; kind-specific placeholders (host-overridable); load groups with `progress` and `wait`; compat IO backend; built-in thread pool; file/memory sources; null adapter; store + cook-on-miss provider; cache-less mode; in-memory registration | A folder of assets loads async through the null adapter within budget; `wait` on a group settles correctly (all ready / some failed / timeout); misuse of `wait` panics with a clear message; tag stats show no steady-state allocation |
| **M4** | Example Vulkan 1.4 adapter + viewer: self-submitting (transfer queue + timeline semaphore), bindless slots with placeholder-then-publish | Viewer starts instantly with placeholders and fills in as assets arrive; a boot group is waited on before the first frame; no upload hitches beyond budget |
| **M5** | Hot reload: polling watcher, re-cook, generation swap, `Changed` events, deferred destroy (glb → textures dependency only) | Editing a glb or PNG updates the viewer in ~1 s without leaks or crashes |
| **v0.5** | Tag after the external project runs on it; API review of `api-friction.md` | Exit criteria below met; v0.6 API changes agreed |

**v0.5 exit criteria:**
- The viewer and the external project load their full asset sets asynchronously, with no frame spikes beyond the configured upload budget.
- Broken assets show a placeholder plus a readable diagnostic, never a crash.
- There are no steady-state allocations while idle, as shown by tag stats.
- A shipping-config build links only `kiln_core` + `kiln_runtime` and loads from a pre-cooked store.
- Neither adapter requires graphics-API calls inside the library.

### 11.4 After v0.5 (roadmap, order may change with feedback)

| Version | Theme | Contents |
|---|---|---|
| **v0.6–0.7** | Quality & pipeline | BCn encoding, Zstd, RDO; `.mesh` payload compression (implement candidate schemes, benchmark ratio and decode MB/s on real assets, pick a default); alpha-coverage mips, channel packing, cubes/arrays; LOD generation (simplifier); config files, presets, rules, sidecars, `--explain`; store index + `--gc`; fuller dependency tracking for hot reload |
| **v0.8** | Streaming | Range requests, progressive mips/LODs (`Partial` state), residency/budget/eviction hooks |
| **v0.9** | Platforms | Target profiles + cross-cooking (ASTC/ETC2 mobile), native async IO backends, native file watchers, macOS; network IO backend for on-device iteration *(discuss)* |
| **v1.0** | Hardening & freeze | `.mesh` v1 freeze; `KILN_NO_EXCEPTIONS` / `KILN_NO_RTTI` in CI; sanitizers everywhere; fuzzing; docs |

Unscheduled: `EXT_meshopt_compression` input, GPU decompression of chunked blobs, pack files, sokol/bgfx adapter examples, EXR, extracting `kiln/core` into a separate utility library.

---

## 12. Working agreement for the agent

- **Before M0 code lands:** propose the concrete dependency list, the vocabulary container set, the error/`Status` design, handles/states, the settings structs, and the adapter interface as short design notes in `docs/`. Get owner sign-off.
- **Stay within v0.5 scope.** When a post-v0.5 feature seems needed, reserve the space (a field, a section ID, an interface hook) rather than building it, and note it in `docs/open-questions.md`.
- Keep PRs small and milestone-scoped. Each PR builds on MSVC, clang-cl and Linux, and passes tests.
- Don't add a dependency, a heavy std header to a public header, exceptions, RTTI, or global mutable state without asking.
- When the spec (`mesh-format-spec.md`) is ambiguous or seems wrong, write the question into `docs/open-questions.md` and propose an answer rather than silently deciding.
- Prefer measured decisions: include numbers (compile time, load MB/s, allocation counts) when arguing for an approach.

---

## 13. Open questions for the owner

1. Project name and license (MIT vs zlib vs Apache-2).
2. C++20 baseline, or C++23 where MSVC allows?
3. Own threading/OS wrapper vs `<thread>`/`<mutex>` confined to `.cpp` files.
4. fastgltf vs cgltf.
5. Format enum: mirror `VkFormat` numerically, or a compact own enum with mapping tables?
6. Should material *remapping* (name → engine material) be a library feature (data-driven table) or strictly the host's job?
7. Config file format for cook settings (TOML vs JSON vs own INI-like). Decision needed by v0.6.
8. Example viewer on raw Vulkan 1.4 or a thin NoGraphicsAPI-style layer?
9. Pack/archive format: in scope for v1?
10. Should textures also expose a `MetaReady` state (extent and format known before pixel data), e.g. for pre-sizing UI layout?
11. Load groups: may a request belong to more than one group, or is one group per request enough?