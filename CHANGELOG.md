# Changelog

All notable changes to this project are documented in this file. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

Pre-1.0: API breaks are allowed but every break is recorded here with migration notes.

## [Unreleased]

### Added
- **No crash dialogs in kiln's own programs:** every `main()` of the tests, tools and examples calls
  `no_crash_dialogs()` (`tools/no_crash_dialogs.h`, target `kiln_no_crash_dialogs`). On Windows a crash,
  `abort()` (`KILN_PANIC`) or a debug CRT assert ends the process with a message and exit code 3 instead
  of a dialog box, so automated runs never hang. The library itself changes no process-wide setting.
- **`.mesh` payload compression** (docs/design/mesh-compression.md): mesh settings `compression` (`None`,
  `Meshopt`, `MeshoptZstd`) and `zstdLevel`, the same sidecar keys, `kiln-cook --mesh-compression` and
  `--mesh-zstd`. The runtime decodes every scheme: `kiln_runtime` now links meshoptimizer (v1.3, fetched
  also by a shipping configure unless the host provides the target). New diagnostic K4024. `kiln-info
  --bench`. `DecodeOptions::alloc` gives decoding its allocator (the runtime passes the context's). The
  reader and `decode_blob()` refuse a blob whose element size breaks meshoptimizer's limits (K4014); the
  writer takes Zstd for a stride meshopt cannot encode (above 256).
- **Runtime texture arrays** (docs/design/runtime-texture-arrays.md, first version):
  `request_texture_array(ctx, TextureArrayDesc{name, layers, ...})` assembles separately cooked 2D
  textures into one `TextureShape::Array` texture at load time, with the usual states, events,
  groups, placeholders and bindless slot. One upload holds every layer, so adapters need no change
  beyond `kArrayTextures`. A layer's manifest change reloads the whole array. New diagnostics:
  K5020 (a bad or conflicting declaration) and K5021 (a layer that is not 2D or differs from
  layer 0). `kMaxTextureArrayLayers` is 2048.
- **Example `kiln-gl-array`** (built with `KILN_EXAMPLE_GL`): a floor of tiles from one texture array
  assembled from `examples/assets/tiles/tile0.png` ... `tile5.png`, with hot reload.
- **Example `kiln-vk-array`** (built with `KILN_EXAMPLE_VK_BASIC`): the same floor through Vulkan,
  with a descriptor set rewritten on kiln's events or, with `--bindless`, through kiln's slot.
- **Examples `kiln-sokol-array` and `kiln-nga-array`:** the texture array floor through sokol_gfx and
  NoGraphicsAPI, with `--dump` and `--verify`. The sokol adapter and readback moved into
  `kiln_sokol_example`; the NGA adapter's textures have `transfer_source` usage (`nga_texture()`).
- **`--verify` in `kiln-gl-array` and `kiln-vk-array`:** reads the array back from the GPU and
  compares every layer and level, byte for byte, with the same tile loaded as a texture of its own;
  exit 1 on a difference. The Vulkan example adapter's images now also have `TRANSFER_SRC` usage.
- `NullAdapterDesc::maxUploadBytes` (0 = unlimited, default): an opt-in staging cap so tests can make
  `begin_upload()` fail with `Unsupported` for an oversized upload, like the GL/Vulkan example adapters.
- **Profiling hooks** (`kiln/profile.h`, docs/design/cook-tracing.md): `ProfileHooks` with zones and
  intervals, set on `ContextDesc::profiler` (the runtime and the cook provider) or `CookEnv::profile`
  (a direct cook); `profile_hooks(ctx)`, `profile_now_ns()`, `ProfileZone`. Off by default.
  `JobBudget::profile` passes them to the image passes. `kiln-cook --trace <file>` and the examples'
  `KILN_TRACE=<file>` write a Chrome trace and a summary (`tools/trace_writer.h`).
- **A warning when another kiln build shares the store:** `manifest.in` records keep the cooker
  version that wrote them (a byte that was reserved, so files stay compatible both ways). A record
  another version wrote cooks again with one log warning per session.
- `CookSession::maxQuality` (default `High`, no cap) caps the resolved texture `quality`.
  `ProviderDesc::maxQuality` sets it for the cook provider.

### Fixed
- **Zstd texture uploads were about 10x slower than needed:** the upload job decoded straight into the
  adapter's staging memory, which is write-combined on GL and Vulkan, and Zstd reads its output back.
  It now decodes into its scratch buffer and copies (warm `kiln-gl`, six uploads: 276 -> 26 ms).
- `destroy()` leaked the declaration (layer names and table) of a texture array the host had not
  released.

### Changed
- **Meshes cook with `Meshopt` payload compression by default** (owner, from the measurements in
  docs/design/mesh-compression.md): about half the payload, decoded at about 2.4 GB/s. Every mesh
  store entry cooks again once (its resolved settings changed). Migration: `compression = "none"`
  (sidecar), `--mesh-compression none` or `MeshCookSettings::compression = None` keeps the raw
  payload (`kPayloadRaw`, one read straight into staging).
- **A glTF's embedded images cook in parallel** (on the cook's job system, up to `CookEnv::maxThreads`).
  Outputs and keys are as before; only the order of diagnostics between images may differ.
  WaterBottle at `Fast` on all cores: 336 -> 157 ms.
- **BC7 `Fast` and `Normal` use Basis Universal's `bc7f`** (extracted into `third_party/basis_bc7f`,
  Apache-2.0): `Fast` the default flags, `Normal` the extended search. `High` keeps `bc7enc`. A
  WaterBottle cook at `Fast` is 5x faster on one thread, `Normal` 1.7x. `kCookerVersion` is 6, so
  every store entry cooks again once.
- **The cook provider encodes at `Fast` by default** (`ProviderDesc::maxQuality = Fast`): a cook on
  a miss and a hot reload take the fast BC encoder. An entry that `kiln-cook` wrote at a higher
  quality stays in use until its sources change. Migration: set `maxQuality = EncodeQuality::High`
  for the old behaviour.
- **Examples layout:** `examples/vk/` holds the shared Vulkan code (`kiln_example_vk`: device,
  adapter, frame plumbing, now `vk_render.{h,cpp}`) and the Vulkan integration examples
  (`main_basic.cpp`, `main_array.cpp`), as `examples/gl/` does for GL; shaders live in
  `examples/vk/shaders/<set>/`. `examples/viewer/` keeps `kiln-viewer` and `kiln-vk-smoke`.
  `examples/vk-basic/` is gone. The shader targets `viewer-shaders` and `vk-basic-shaders` are one
  target, `vk-shaders`. CMake options are unchanged.

## [0.6.0] - 2026-09-30

Stores that cook only what changed: artifacts named by a build key over every input, one manifest
for all target profiles, `kiln-cook --gc` and `--export`. Also target profiles and PBR material
factors. The named store layout is gone: delete old stores and cook again.

### Changed
- **Breaking (format, runtime, cook, tools): one flat store with a shared manifest** (owner,
  2026-09-30; store-manifest.md). A store is `manifest.dir` (what the runtime reads: every target
  profile's entries and index, format `KMAN` 0.1), `manifest.in` (the cook's input records of every
  profile, never shipped), `manifest.lock` (one writer per store, K3009) and the artifacts at
  `<store>/<26>`: the build key in base32 (`hash128_base32()`), no extension, no subdirectories. Profiles share the store; a writer edits its own profile and keeps the others, and
  drops only its own when it was cooked for another definition of it. Input records keep no paths
  (inputs are found next to the unit's source as it is found now), so a store survives moved
  sources. A file whose size or time changed is hashed before it counts as changed: unchanged
  content updates the record and cooks nothing. Renamed: `kiln/catalog.h` is `kiln/manifest.h`
  (`ManifestView`, `ManifestProfile`, `ManifestEntry`, `manifest_file_path()`, `kDiagManifest*`),
  `kiln/cook/catalog.h` is `kiln/cook/manifest.h` (`write_manifest()`, `kDiagStoreLocked`);
  `artifact_file_path()` takes no kind; `kiln-info` reads `manifest.dir`; the fuzz target is
  `kiln_fuzz_manifest_read`.
  - Migration: delete the store and cook again (`catalogs/`, `inputs/` and `artifacts/` are not
    read any more). Code that named the old types uses the new names above.
- **Breaking (runtime, cook, tools): the named store layout is removed** (owner, 2026-09-30); the
  manifest store is the only one. Gone: `StoreLayout` and `ContextDesc::storeLayout`,
  `store_layout()`, `store_file_path()`, `kiln-store.txt` (`StoreProfile`, `parse_store_profile`,
  `read_store_profile`, `bind_store_profile`), `store_key()` and `store_file_name()`,
  `CookProvider::cook` (the provider always installs `prepare`), and `--layout` in `kiln-cook`,
  `kiln-headless` and `kiln-viewer`. The store poller watches only the manifest; a reload without
  it reads the manifest again when it starts. A `prepare` that returns Ok with neither bytes nor a
  key leaves the manifest entry as it is. K3008 now means a provider whose target is not the
  context's profile. An empty `ContextDesc::storeDir` is a context without a store.
  - Migration: cook named stores again into a new directory (`kiln-cook <sources> -o <store>`);
    kiln deletes no store. A host `CookProvider` implements `prepare` instead of `cook` (return the
    bytes with a zero key, or Ok with nothing to use the manifest). Tests and tools that read
    `tests/golden` as a store use `<build>/tests/samples/golden-store`, which `kiln_tests
    GoldenStore.Build` (the ctest fixture `golden_store`) writes.
- **Breaking (runtime, tools): `kiln-cook` writes the store** (store-manifest.md, Phase A step 6):
  artifacts and the manifest. It cooks only the sources whose recorded inputs changed; `--verify`
  compares every input's content, for CI and shipping builds. `--hashed` is gone; `--map` prints
  `<name>\t<file>\t<build key>`. `cook_cli_main` takes a `policyVersion`. `kiln-info` reads the
  manifest (`--check` verifies every artifact).
  - Migration: cook the store again into a new directory; kiln deletes no store. A cook provider
    needs its `target` to be the context's profile.
- **Breaking (cook): target profiles replace `blockFamily`.** `TargetProfile::blockFamily` and the
  `BlockFamily` enum are gone (`blockFormats` replaces them); the default target is `compat`, not
  `desktop`, so one-channel masks are BC5 instead of BC4 by default. `kiln-cook --block` is gone
  (`--target uncompressed` replaces `--block none`). An explicit `encoding` the profile does not
  have is a K3002 error (it was clamped with K3003). `kTargetSchema` is 2, so every store key and
  `cookHash` changed once.
  - Migration: `blockFamily = None` becomes `blockFormats = 0` or `kUncompressedTarget`;
    `blockFamily = BC` becomes `kDesktopTarget` (or keep the default, `compat`). Delete existing
    stores once: a store with cooked files and no `kiln-store.txt` is refused (K3008).
- **Breaking (format, cook): `.mesh` 0.5, materials carry their PBR factors.** `MaterialSlot` grows
  from 32 to 80 bytes: `baseColorFactor[4]`, `emissiveFactor[3]` (`KHR_materials_emissive_strength`
  folded in), `metallicFactor`, `roughnessFactor`, `normalScale`, `occlusionStrength` (glTF's
  defaults when the source has none), then a reserved `u32`. Materials that differ only in factors
  are no longer merged. `kCookerVersion` is 5; `kiln-info` prints the factors.
  - Migration: a 0.4 `.mesh` fails with `VersionMismatch` (K4002); delete existing stores so they
    re-cook (a named store file is used while it exists, open-questions R9). Renderers that read
    `MaterialSlot` get the factors from the same record.
- **Cook threads** (owner, 2026-09-30): `CookEnv::maxThreads` and `kDefaultCookThreads` default to
  6 (was 3), and every cap is lowered to the hardware threads. A cook on miss of WaterBottle
  (four 2048x2048 BC textures) took 2.5 s with 3 threads; `kiln-cook` needs 1.2 s.
  - Migration: none. A host that wants the old budget sets `maxThreads = 3`.
- **Examples:** `kiln-gl` and `kiln-gl-bindless --dump` wait for the previous frame, as a swap
  does. Without that, the driver queued frames until a later GL call blocked for seconds (a cold
  start took 8.7 s against 2.9 s in a window).

### Added
- **Target profiles** (docs/design/target-profiles.md). A profile is the set of block formats a
  target samples: `TargetProfile::blockFormats` (a `block_format_bit()` set), with the built-in
  `kCompatTarget` (the default: BC3, BC5, BC6H, BC7, which every example backend samples),
  `kDesktopTarget` (adds BC4 and BC1) and `kUncompressedTarget`; `target_profile(name)`;
  `kiln-cook --target compat|desktop|uncompressed`. The usage table picks the first format a usage
  prefers from the profile (a 1-channel mask: BC4, else BC5).
- `create()` checks the store's profile against the adapter and fails with K5018 when the adapter
  cannot sample one of its formats (`ContextDesc::allowUnsampledFormats` makes it a warning).
  `diag_sink(ctx)` returns the context's diagnostic sink. `unsampled_block_formats(adapter)`
  (`kiln/adapter.h`) reads the set an adapter cannot sample.
- Examples: every integration example cooks with the default profile into one `example-store`.
- Examples: `example-store` and the viewer's stores are manifest stores (store-manifest phase A step
  7): the backends share the artifacts cooked by the first one to run. Delete a build tree's old
  `example-store` once.
- Examples: every renderer (the viewer, `kiln-gl`, `kiln-gl-bindless`, `kiln-sokol`,
  `kiln-vk-basic`, `kiln-nga`) shades with the `MaterialSlot` PBR factors: a texture times its
  factor, the factor alone without the texture (glTF's rules), so untextured materials get their
  authored color. `ex::material_factors()` and `vkx::material_uniforms()` pack them; the Vulkan
  examples read them from a material table in the frame uniforms (`DrawPush::material`), the others
  per draw. Scenes with all factors at 1 render as before (WaterBottle: identical pixels).
- **Build keys** (store-manifest.md, Phase A step 1): `Hash128`, `xxh3_128()`, `hash128_hex()` and
  `hash128_base32()` (`kiln/manifest.h`, from the xxhash zstd vendors); `build_key()`
  (`kiln/cook/manifest.h`) hashes
  the cooker version, asset kind and name, target, resolved settings and the content of every
  input file a cook read: the source, its sidecar (or its absence), a `.gltf`'s buffers. The cook
  provider and `kiln-cook` share one cook path that records those inputs.
- **Store manifest format 0.1** (step 2): `ManifestView` validates a manifest in memory and
  `ManifestProfile` looks names up in one profile (binary search, no allocation); `write_manifest()`
  writes one, `manifest_file_path()` and `artifact_file_path()` name the files,
  `check_profile_name()`. Diagnostics K4201-K4209; fuzz target `kiln_fuzz_manifest_read`.
- **Store writing** (step 3, cook-internal): artifacts written once (K3010 when a cook gives other
  bytes for an existing key); the manifest and the input records rewritten by a temporary file and
  a rename; one writer per store, an OS lock on `manifest.lock` (K3009).
- **The runtime reads the store** (step 4): `ContextDesc::profile` (default `compat`). `create()`
  reads and validates `manifest.dir` (a malformed one fails with its K42xx) and checks the
  profile's formats against the adapter (K5018). A request looks its name up at dispatch and loads
  the artifact; a miss goes to the cook provider, else K5001, or K5019 when there is no manifest or
  no such profile in it. The store poller (`HotReloadDesc::watchStore`) watches the manifest and
  reloads the assets whose entry names another artifact. `store_profile()` gives the profile.
- `kiln-cook --watch [--timeout <s>]`: after cooking, keeps cooking the sources that change or
  appear, twice a second, and writes the manifest once per round. With a read-only app that watches
  the store (`HotReloadDesc::watchStore`, no cook provider), this is a dev loop with the cooker in
  another process. A failed source is reported once and cooked again when it changes; the exit
  code counts the sources still failing. Every `kiln-cook` run also writes the manifest at most once
  a second while it cooks, so a watching app fills in during a long run.
- **Store maintenance** (store-manifest phase B): `manifest.in` records the roots its writers used
  (a directory relative to the store when it can be), so `kiln-cook -o <store>` without inputs
  scans them again: new and changed sources cook, and units whose source under a scanned
  directory is gone leave the manifest. The cook provider records the context's roots too.
  `kiln-cook --gc -o <store> [--dry-run]` deletes the artifacts no profile references and leftover
  temporary files, and nothing else (K3009 while a writer runs). `kiln-cook --export <dir> -o
  <store> [--target <profile>]` writes a runtime-only store (`manifest.dir` and its artifacts,
  checked while copied) into an empty directory. `manifest.in` is minor 3: an older one is dropped
  once (its sources are checked again).
- **Store recovery and consistency** (owner's audit of PR #3):
  - Input records keep their outputs' build keys; a record whose keys differ from `manifest.dir`
    (a crash between the two writes) is dropped when the store opens, so its unit cooks again
    instead of reading as up to date, with or without `--verify`.
  - A cook removes the entries of its unit it did not make (the unit's name and `<unit>#...`),
    from the names alone: a lost `manifest.in` no longer leaves a glb's old images addressable.
  - A root that is missing or cannot be listed in full drops nothing, and `kiln-cook` exits 2; a
    source is gone only when looking it up says "not found".
  - `kiln-cook --watch` retries a failed source when any file the failed cook read changes (a
    `.gltf`'s buffer too), not only the source and its sidecar.
  - `--target` accepts any valid profile name; cooking still needs a built-in one (exit 1
    otherwise), and `--export --target <name>` exports a custom profile of the store.
- **Breaking (runtime API): `CookProvider::prepare` takes a `PrepareMode`.** `request_reload()`
  passes `PrepareMode::Recheck`: the provider checks the asset's sources again even when it checked
  them earlier in the session, so a host with its own file watcher gets an edit by calling
  `request_reload()`. Loads and reloads after a manifest change pass `Normal`.
  - Migration: a host's own `prepare` (or a wrapper of the installed one) takes the new parameter
    after `assetPath`, and passes it on when it wraps.
- **The cook provider on the store** (step 5): `CookProvider::prepare`, called before every load of
  a file asset (hit or miss), names the asset's artifact or returns freshly cooked bytes.
  `install_provider` checks the context's profile (K3008), takes the store lock in Disk mode
  (K3009) and installs `prepare`. Each source is checked once per session by the size and time of
  its recorded inputs (a differing file is hashed; only a content change cooks); a changed host
  setting (`ProviderDesc::policyVersion` for the policy) only re-checks the keys from the recorded
  hashes. The source poller watches the checked sources, including a `.gltf`'s buffers, and
  rewrites the manifest once per round.

## [0.5.0] - 2026-09-29

The first tagged release: a usable async asset loader (milestones M0-M5), with BC and Zstd
textures from the v0.6 work already in. The API is not stable; every break below has migration
notes.

### Added
- **Runtime: Zstd-supercompressed KTX2** (docs/design/bcn-encoding.md, step 6). The reader accepts
  `supercompressionScheme = 2` (BasisLZ and Zlib stay K4107); `Ktx2View::supercompressed()` and
  `Ktx2View::decode_level(level, out, alloc, diag)` give a level's texels, and the loader decodes
  each level straight into the adapter's memory. New K4110 (`kDiagKtxLevelDecode`) for a level
  that is not one Zstd frame of its size. zstd 1.5.7 is vendored (`third_party/zstd/`): the
  decoder (`kiln_zstd`) is `kiln_runtime`'s first third-party code and installs with it; the cook
  links the encoder (`kiln_zstd_enc`).
- **Cook: Zstd supercompression, on by default.** New `TextureCookSettings::supercompression`
  (`Supercompression`: `None`, `Zstd`; default `Zstd`) and `zstdLevel` (1..19, 0 = 3;
  `fastPreview` uses 1). Sidecar keys `supercompression` and `zstdLevel`, `kiln-cook --zstd <level>`
  (0 = off), `ktx2::WriteDesc::zstdLevel`. A KTX2 source with Zstd levels passes through.
  `kiln-info` prints each level's stored and texel sizes, and `--check` decodes every frame.
  On the example assets the uncompressed store shrinks 6.5x and the BC store 2.9x, for a few
  percent more cook time.
  - Zstd only when it pays: the cook keeps a texture plain unless Zstd saves 10% of the file in
    4 KiB disk blocks (`kZstdMinSaving`, `ktx2::WriteDesc::zstdMinSaving`), so small files and
    textures that barely compress load without decoding. `kCookerVersion` is 4, so every store key
    and `cookHash` changed once (mesh files only in their header's `cookHash`).
- **Cook: BC1/3/4/5/7 textures** (docs/design/bcn-encoding.md, rollout step 3).
  - New `TextureCookSettings::encoding` (`TextureEncoding`: `Auto`, `Uncompressed`, `BC1`, `BC3`,
    `BC4`, `BC5`, `BC6H`, `BC7`) and `quality` (`EncodeQuality`: `Fast`, `Normal`, `High`).
  - New `TargetProfile::blockFamily` (`BlockFamily`: `None`, `BC`).
  - With `BC`, `Auto` gives BC7 for color, UI and ORM, BC5 for normals, and BC4 or BC5 for 1- or
    2-channel masks, and BC6H (unsigned) for HDR. Height and LUT stay uncompressed.
  - Encoders: `rgbcx` (BC1/3/4/5) and `bc7enc` (BC7) from bc7enc_rdo, vendored in
    `third_party/bc7enc_rdo/`; for BC6H, a scalar C++ port of the ISPC Texture Compressor's
    encoder (`third_party/ispc_bc6h/`), byte-identical to the ISPC original. Block rows are encoded in parallel on the cook's job pool, with the
    same bytes on any thread count.
  - K3002 for an encoding the usage cannot take; K3003 when a target without the BC family turns a
    BC encoding into `Uncompressed`. `CookSession::fastPreview` sets `Fast`.
  - Sidecar keys `encoding` and `quality`. `kiln-cook --block none|bc` and `--quality`.
    `CookStats::encodeUs`.
  - The built-in `desktop` target keeps `blockFamily = None` until the example adapters upload BC
    textures (rollout step 5). Existing store keys and goldens are unchanged, because the new fields
    are hashed only when they differ from their defaults.
- **Cook:** the KTX2 writer (`kiln::ktx2::write`) accepts BC1-BC7 (UNORM, sRGB, SNORM, BC6H
  UFLOAT/SFLOAT) with mips, cube faces and array layers. Its DFDs match `ktx create` byte for byte;
  the new corpus files `tests/corpus/ktx2/generated/bc*.ktx2` check that in the round-trip test,
  and `ktx validate` checks kiln's output. ETC2 and ASTC are still rejected with K4103. First step
  of block compression (docs/design/bcn-encoding.md); nothing encodes BC yet.
- Every example adapter takes an `Allocator` in its desc and reports the shared
  `ex::AdapterStats` (`gl_adapter_stats`, `sokol_adapter_stats`, `nga_adapter_stats`,
  `vkx::adapter_stats`): live objects, pending and failed uploads, bytes committed, staging use and
  size, and why `begin_upload` said `Busy` (staging ring, GPU heap, upload records). Every example
  logs them at exit (`ex::log_adapter_stats`). `NgaAdapterDesc` gains `maxObjects` / `maxUploads`
  (were constants). Migration: `vkx::AdapterStats` is gone; `busyReturned` is now `busyStaging`,
  `bytesUploaded` `bytesCommitted`, `uploadsInFlight` `uploadsPending`.
- `examples/adapter_support` (`kiln_example_adapter_support`): CPU-only helpers the example
  adapters share, starting with `ex::StagingRing`, a FIFO allocator for upload staging memory
  whose ranges may be released in any order, and `ex::UploadPool<T>`, upload records with one
  `UploadState` each (Free, Writing, Committed, InFlight, Complete, Failed; transitions asserted)
  and generation-checked tokens, and `ex::CommitQueue`, the bounded handoff of committed uploads
  from kiln's workers to `flush` (push from any thread, `take()` everything in commit order, the
  GPU work outside its lock). `kiln-gl` and `kiln-nga` use all three, `kiln-sokol` the pool and
  the queue, in place of their copies of the same code, their per-upload `used` / `committed` /
  `flushed` / `done` / `failed` flags and their `committed` / `flushing` vector pairs; the tests
  (`kiln_adapter_support_tests`) run with the default CI.
- Hardening: `checked_add` / `checked_mul` and the `NothrowStorable` concept (`core.h`);
  `alloc_array`, `Vec` and `HashMap` panic on size overflow instead of allocating short, and
  `alloc()` panics on an alignment that is not a power of two. CMake `KILN_SANITIZE` (e.g.
  `address,undefined`, clang or gcc) and `KILN_FUZZ` (libFuzzer targets in `fuzz/` for the `.mesh`
  and KTX2 readers and the `.hdr` decoder, run locally); the manual `extended` workflow runs the
  tests under ASan and UBSan.
- `placeholder_object(ctx, kind, shape)`: the placeholder `GpuObject` of a texture kind and shape,
  for hosts that must bind something where a material has no texture. `kiln-sokol` and
  `kiln-vk-basic` use it (the latter no longer needs `descriptorBindingPartiallyBound`).
- `texture_asset_name(meshName, view, binding, out, cap)`: the texture asset name a `.mesh`
  texture binding refers to (embedded name as stored, external URI resolved in the mesh's root).
  Replaces the helper every example wrote.
- The integration examples (`kiln-gl`, `kiln-gl-bindless`, `kiln-sokol`, `kiln-vk-basic`, `kiln-nga`)
  take no arguments: they show the reference scene (WaterBottle under the HDR test sky) from
  `examples/assets`, cook into the build tree's `example-store` and hot reload. The only option is
  `--dump <file.png>`; `+` / `-` change the exposure. Migration: drop `--store`, `--source`, `--root`,
  `--sky`, `--exposure`, `--width`, `--height`, `--watch`, `--offscreen`, `--timeout` and the model
  name. The `viewer-assets` target (the demo models) now exists whenever a windowed example is built.
- `kiln-nga` (`examples/nga`, CMake option `KILN_EXAMPLE_NGA`, OFF everywhere, its own CI job): the
  scene through NoGraphicsAPI. Mesh payloads are written by kiln straight into CPU-visible GPU
  memory and read through GPU pointers; textures are descriptor heap indices behind a CPU slot table.
  Fetches NoGraphicsAPI, builds the Vulkan loader from source (no SDK) and downloads Slang. Built,
  not yet run: it needs `VK_EXT_descriptor_heap`.
- `kiln-vk-basic` (`examples/vk-basic`, CMake option `KILN_EXAMPLE_VK_BASIC`, ON in the presets): the
  scene on Vulkan 1.4 without bindless, a descriptor set per material and frame slot rewritten on
  kiln's events. The viewer's Vulkan code is now the library `kiln_example_vk`: its adapter takes
  `AdapterDesc::bindless` (false: no `acquire`; `adapter_texture()` gives the image view), and its
  renderer takes a host's SPIR-V and material set layout (`RendererDesc`).
- `EventKind` documents which events may change what `gpu()` returns (Ready, Changed, Failed).
- `kiln-sokol` (`examples/sokol`, CMake option `KILN_EXAMPLE_SOKOL`, ON in the presets): the scene
  through sokol_gfx with sokol_app owning the main loop (D3D11 on Windows, GL on Linux, Metal on
  macOS). Its adapter writes uploads into per-upload CPU memory and makes the images and buffers in
  `flush`. New example-only dependencies: the sokol headers and the sokol-shdc binary, both at
  pinned commits (the binary checked by SHA-256). The command line of the integration examples
  moved to `examples/common` (`ex::Options`, `ex::parse_options`).
- `kiln-gl-bindless` (`examples/gl/main_bindless.cpp`, CMake option `KILN_EXAMPLE_GL_BINDLESS`, ON in
  the presets): `kiln-gl` with `ARB_bindless_texture`. The GL adapter gains a bindless mode
  (`GlAdapterDesc::bindless`): `acquire` gives each texture a slot in a persistently mapped table
  of resident handles, `publish` writes the real handle, and retired handles and slots wait for a
  fence. The two GL examples share `gl_util` and render the same pixels.
- `Adapter::flush` (optional): kiln calls it at the start of every `pump()` (so in every `wait()`
  loop) and in `create()`'s placeholder spin, on that thread. An adapter for an API that must be
  called on one thread (GL, sokol) does its GPU work there. `wait()` and the placeholder wait now
  accept `kSelfSubmitting` or `flush`. The field sits between `destroy_deferred` and `caps`: code
  that fills `Adapter` with designated initializers keeps compiling; code that relies on aggregate
  order without designators must add it.
- `texture_level_layout(TextureDesc, CopyConstraints, offsets, pitches)` in `kiln/adapter.h`: the
  level offsets and row pitches kiln writes into a texture upload, so an adapter no longer copies
  the rule. The example Vulkan and GL adapters use it.
- `kiln-gl` (`examples/gl`, CMake option `KILN_EXAMPLE_GL`, ON in the presets): the first
  integration example (`docs/design/integration-examples.md`). One model and a cube sky through
  OpenGL 4.6 core with textures bound per draw; its adapter writes uploads into a persistently mapped
  staging ring on kiln's workers and does the GL work in a host-side flush on the GL thread. Own GL
  loader, no new dependency. `examples/common` holds what the windowed examples share (logging,
  orbit camera, PNG dumps) and now fetches GLFW for all of them.
- `VertexProfile::Float` (`profile = "float"` in a sidecar, `kiln-cook --profile float`): every
  vertex attribute as a plain float vector (position, normal `R32G32B32_SFLOAT`, tangent
  `R32G32B32A32_SFLOAT`, UVs `R32G32_SFLOAT`; color stays `R8G8B8A8_UNORM`), 48 B/vertex for the
  usual attributes. No shader decoding; for simple renderers and the integration examples. The
  default `TargetProfile::maxVertexProfile` is now `Float`, which changes the target hash: every
  cook key and the `cookHash` stored in cooked files change (payloads are unchanged), so existing
  stores re-cook once.
- Texture-only builds (`docs/design/texture-only.md`): CMake option `KILN_MESH` (default ON). With
  OFF, `kiln_cook` builds without the glTF importer and mesh cooker, and cgltf, MikkTSpace and
  meshoptimizer are neither fetched nor built. `cook_mesh` stays declared and fails with
  `Unsupported` and the new K1021; `kiln-cook` still cooks the other inputs. The `viewer-demo`
  target and the glb-based CTests need `KILN_MESH=ON`.
- `AdapterCaps::kMeshes`: the adapter accepts mesh payloads. Without it kiln never acquires or
  uploads a mesh, and a mesh request fails with K5004. **Break:** an adapter that loads meshes must
  now set the bit. Migration: add `kMeshes` to `Adapter::caps` (the null adapter and the example
  Vulkan adapter do).
- `kiln-viewer --tonemap auto|none|aces` and `--exposure <ev>`: a display curve for HDR content.
  `auto` (default) applies ACES only once an HDR (float) `--sky` has loaded, so LDR scenes render
  unchanged. `FrameUniforms` gains `tonemap` (now 112 bytes); `mesh.frag` and `sky.frag` apply it.
- HDR textures (`docs/design/hdr-textures.md`): Radiance `.hdr` sources through kiln's own decoder
  (`decode_hdr`, `is_hdr`; flat and run-length scanlines, standard orientation), usage `Hdr`
  cooking to `R16G16B16A16_SFLOAT` with f32 mips and `float_to_half` / `half_to_float`
  (round to nearest even, saturating). `Image::bitsPerChannel` may be 32 (f32). A `.hdr` source
  implies usage `Hdr` and a linear color space; the provider and `kiln-cook` accept `.hdr`,
  including cube and array strips. New warning K2011 for an integer source with usage `Hdr`. No
  new dependency.
- `kiln-viewer --offscreen` stops when the scene settles (every mesh and texture Ready or Failed)
  by default, capped by the new `--timeout <s>` (exit code 1 when it expires); `--at <ms>` stops at
  the first frame at or after that time; `--frames N` still renders exactly N frames. The default
  used to be 60 frames, which could dump placeholders. Log lines carry milliseconds since start.
- `kiln-headless` drops `--frame <ms>`: it pumps until settled and rests 1 ms only after an idle
  pump. Progress lines lose their frame number; the summary reports milliseconds and pumps.
  Migration: remove `--frame`.
- `kiln-viewer --sky <name>` draws a cube texture behind the scene (a sky pass with
  `shaders/sky.{vert,frag}`). The example Vulkan adapter now takes cube and array textures: one
  bindless binding per `TextureShape` (2D, cube, array) sharing a slot index, cube-compatible
  images, and `kCubeTextures | kArrayTextures` (`docs/design/texture-shapes.md`, step 5).
- Cube and array textures from one strip image (`docs/design/texture-shapes.md`, steps 3 and 4):
  `TextureCookSettings::shape` (`CookShape`) and `slices` cut a vertical strip into faces or layers,
  each with its own mip chain and size cap; a bad strip is the new K2010. Name rules stack
  (`rock_array_n.png` is an Array of normal maps) and gain a shape: `NameRule::shape`,
  `hints_from_name`, default rules `_cube` and `_array`. Sidecar keys `shape` and `slices`. A KTX2
  source whose shape differs from a non-Auto `shape` is K2004. Texture settings schema 2: the
  settings hash changes, so stores re-cook textures.
- KTX2 cube and array textures in the cook (`docs/design/texture-shapes.md`, step 2):
  `ktx2::WriteDesc` gains `layers`, `faces` and `isArray`; a cube or array KTX2 source passes
  through with its shape (a volume or cube array is still K2004); new
  `TargetProfile::maxArrayLayers` (default 2048), where more layers is K2004 rather than a clamp.
  CTest runs `ktx validate` on cube and array output and round-trips the corpus cube and array.
- Cook: per-asset `.kiln` sidecar files (`wall_n.png.kiln`, `chair.glb.kiln`) set cook settings for
  one standalone texture or mesh. The syntax is a strict TOML subset parsed by kiln itself (no
  dependency); keys are the settings field names. `apply_sidecar()` in the new
  `kiln/cook/sidecar.h`; the provider and `kiln-cook` read sidecars, and the source poller re-cooks
  when one is added, edited or removed. New diagnostics K3005 (syntax) and K3006 (key or value).
  Syntax and keys: `docs/design/settings.md`, "Sidecar files".
- Cook: name rules give a standalone texture source its usage from its file-stem suffix
  (`wall_n.png` is a linear normal map, `crate_orm.png` is ORM). `NameRule`, `kDefaultNameRules`
  and `usage_from_name` in `kiln/cook/settings.h`; `ProviderDesc::nameRules` (default: the
  built-in table, copied at install) and `kiln-cook` apply them when the usage is `Auto`. Before,
  every standalone texture was cooked as sRGB color. Textures referenced from a glTF still take
  their usage from the material slot.
- JPEG texture sources, embedded in glTF (`image/jpeg`) or loose `.jpg`/`.jpeg` files, through the
  JPEG decoder of the already vendored wuffs (no new dependency). WebP sources (`.webp` files and
  `EXT_texture_webp`) behind the new CMake option `KILN_WEBP`, default OFF. `kiln/cook/image.h` adds
  `decode_jpeg`, `decode_webp`, `decode_image` (picks the decoder by signature), `is_jpeg`,
  `is_webp`, `is_lossy_image` and `webp_decode_enabled`. New warning K2009 when a Normal or Height
  texture comes from a lossy source. The cook provider and `kiln-cook` accept the new extensions.
- `viewer-assets` fetches a sixth CC0 model, DiffuseTransmissionTeacup, whose GLB embeds JPEG base-color
  textures next to PNG maps; `viewer-demo` shows it.
- M5 hot reload (`docs/design/hot-reload.md`). Runtime: `request_reload()` reloads a store-backed
  asset while the old version stays servable; success bumps the content version, publishes the new
  object, sends the old one to `destroy_deferred` and emits `Changed` (or `Ready` from `Failed`, which
  also moves the asset from `failed` to `ready` in its load group); failure keeps the old version with
  K5010; memory-registered assets give K5012. `ContextDesc::hotReload.watchStore` starts a store
  poller (`KILN_HOT_RELOAD` builds; otherwise K5011). `IoBackend::stat` is a new optional entry, the
  compat backend implements it and opens files with `FILE_SHARE_DELETE` on Windows. Cook side:
  `ProviderDesc::watchSources` / `pollMs` start a source poller that re-cooks changed sources (a glb
  with its embedded textures, a PNG or KTX2 on its own) into the store; `cook::store_write` gains
  `bool overwrite = false`. `kiln-viewer --watch` and `kiln-headless --watch` turn both on.
- `ContextDesc::workerPriority` passes a `ThreadPriority` to the built-in pool, so a host that lets kiln
  create the pool can still keep loading below its own threads.
- M4: `kiln-viewer` draws cooked meshes through the example adapter with one pipeline per vertex
  layout (zero-buffer inputs and specialization constants for attributes a layout lacks), dynamic
  rendering and synchronization2, two frames in flight, and frame submits that wait on the adapter's
  upload watermark. Boot meshes are waited on as a group; their base-color textures stream in under
  `--budget-mib`, so placeholders show first. Orbit camera in the window; `--offscreen --frames N
  --dump out.png` renders headless; `--source` enables cook-on-miss. Boot models are scaled to a bounding radius of 1 and placed 2.5 units apart
  (`--no-fit` keeps native sizes). Demo models: `viewer-assets`
  fetches five CC0 Khronos glTF models at a pinned commit with SHA-256 checks into
  `examples/assets/khronos/` (git-ignored; `examples/assets/README.md`).
- M4: `examples/viewer`. Vulkan 1.4 device bring-up (volk, optional validation layer),
  the example `kiln::Adapter` (dedicated transfer queue, timeline-semaphore tokens, host-coherent
  staging ring with `Busy` back-pressure, bindless slots with placeholder-then-publish, deferred
  destroy by frames in flight, per-format `static_assert`s against `VkFormat`), and `kiln-vk-smoke`,
  which loads cooked assets through the adapter with no window. Dependencies, examples only:
  Vulkan-Headers, volk and GLFW through pinned FetchContent (`docs/design/viewer.md`,
  `third_party/README.md`). `KILN_BUILD_VIEWER=ON` in every preset; CI compiles the viewer targets.
- `tools/cli.h`: a small option-table argument parser shared by `kiln-cook`, `kiln-info` and
  `kiln-headless` (not installed, no allocation, no exceptions). Options are declared once with
  designated initializers; `--help` prints usage generated from the same table; values accept
  `--opt value` and `--opt=value`; numbers are range-checked and choices validated. Every existing
  option and exit code is unchanged.
- `ThreadPoolDesc::priority` (`ThreadPriority::Normal` / `Low` / `High`): the host's hint for where
  kiln's workers sit relative to its own threads. The built-in pool applies it per worker on
  Windows (`SetThreadPriority`) and Linux (per-thread nice); other platforms ignore it for now.
- Cook thread budget: `CookEnv::maxThreads` (default 3, caller included; 1 = inline, 0 = no cap)
  limits how many pool threads one cook may occupy; the image functions take `JobBudget { jobs,
  maxThreads }` in place of the bare `JobSystem const*`. `kiln-cook --threads <n>` sets both the
  pool size and the budget. The renormalize kernel's 8-bit first step comes from a 256-entry table,
  and the normal-map downsample renormalizes on the same SSE2 path. Output is unchanged.
- Cook kernels, steps 4 and 5: `cook_mesh` builds and quantizes each (part, LOD) as a task over
  `CookEnv::jobs` and merges the results sequentially in traversal order, so cooked bytes and the
  diagnostic order are unchanged with or without a pool (tested with 1 and 8 threads); the mesh
  `CookStats` fields are summed task time and can exceed `totalUs` with a pool. The renormalize
  kernel (normal maps: level 0 prepare and `renormalize()`) has an SSE2 path on x64 that is
  bit-identical to the scalar path and about 2.8x faster at 4096x4096. `KILN_ARCH_X64` /
  `KILN_ARCH_ARM64` join `kiln/core.h`.
- Cook kernels, step 3: `kiln::cook::CookEnv { alloc, diag, jobs }`; when `jobs` is set, the image
  passes (prepare, flip green, renormalize, and the level 0 to 1 downsample) split by row bands over
  the job system through an internal helping `parallel_for` that never deadlocks inside a worker.
  Output is byte-identical with or without a pool. `kiln::jobs(ctx)` exposes the context's pool and the
  cook-on-miss provider passes it; `kiln-cook --threads <n>` and `kiln_bench_image --threads <n>`
  select the pool size. Image functions gain a trailing `JobSystem const* jobs = nullptr`.
- Cook kernels, step 1 and 2 of docs/design/cook-kernels.md. `kiln::cook::CookStats` on
  `CookedTexture` / `CookedMesh` gives per-stage microseconds (decode / prepare / mips / write for
  textures; import / build / tangents / optimize / pack / write for meshes) and `kiln-cook --verbose`
  prints them. `kiln_bench_image` (tests/, manual, not a CTest) benchmarks the image kernels and
  `cook_texture` end to end. `src/cook/kernels.h` holds the image kernels as format-specialized
  templates with a kernel table; `linear16_to_srgb8` uses a 64 KiB table instead of a binary search;
  `kiln::cook::prepare_image` does convert + flip green + renormalize in one pass. Cooked bytes are
  unchanged (`kCookerVersion` not bumped); `downsample_2x` on 4K RGBA8 is about 5x (linear) and 9x
  (sRGB) faster. `KILN_HOT` and `KILN_RESTRICT` join `kiln/core.h`.
- `examples/headless`: a GPU-free walkthrough of the runtime API (null adapter, requests, a load
  group, `pump()` per frame, events, metadata queries, cook-on-miss when built with `kiln_cook`)
  that logs every step. `--slow` / `--latency` add artificial IO and cook delays so large files
  visibly take time. Built by every preset (`KILN_BUILD_EXAMPLES=ON`) and run as a CTest smoke test.
- `kiln::cook_provider(ctx)` returns the installed provider so a host can wrap it.
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
- Runtime/cook: `destroy(ctx)` with a cook provider still installed leaked the provider and left
  its `Context*` in the provider registry. `CookProvider` gains `release`, which `destroy()` calls
  after the last load; `install_provider` sets it, so `uninstall_provider` is now optional.
- `.mesh` reader: a section table offset near 2^64 wrapped the bounds check, and `open()` read
  outside the buffer (found by `fuzz_mesh_read`).
- `Vec::push_back` / `emplace_back` / `append` / `resize(n, fill)` and `HashMap::try_emplace` read
  a freed buffer when the argument referred into the container and the call made it grow.
- The `.hdr` decoder allocated the whole image before checking the file could hold it: a
  30-byte header could ask for 3 GB.
- The store writer ignored `fclose` errors, so a failed close could still publish the file.
- MikkTSpace (vendored): a shift by 32 in its quicksort seed (undefined behavior, found by
  UBSan); the output is unchanged. Recorded as a local change in `third_party/README.md`.
- Example Vulkan adapter: a small upload no longer waits for a larger one that began earlier. The
  timeline value is given at `commit_upload` instead of `begin_upload`, so submission follows
  commit order; the upload token is now the object handle (index and generation). A small sky
  texture now becomes Ready about 5 ms before a 22 MB texture requested with it, instead of on the
  same frame. `kiln-vk-smoke` checks the order before loading and exits 1 if it breaks.
- `PumpStats::uploadsStarted` and `uploadBytes` counted a Busy retry again each time it was
  re-dispatched, so a run with back-pressure could report many times the bytes actually uploaded.
  Retries still consume the per-pump budget and show up in `busyRetries`.
- KTX2 writer: the alpha sample of sRGB formats carried the EXPONENT qualifier (0x20) instead of LINEAR (0x10); found by `ktx validate`.
- Cook: external glTF image URIs were not percent-decoded, so a texture such as `my%20wood.png`
  could not be read (K1005 / "cannot read external texture"). `TextureRef::uri` is now the decoded
  relative path, as the `UriResolver` already received for buffers.

### Changed
- **Breaking (cook, format): cooked textures are Zstd-supercompressed by default** (see Added).
  - The default settings hash, so every default texture store key and `cookHash`, changed once;
    with `supercompression = None` the hash is the one from before, and so is the file.
  - `Ktx2View::level_data()` of a supercompressed file returns the Zstd frames: tools that read
    texels from it call `decode_level()` (plain files decode by copying).
  - The texture goldens are Zstd files now.
  - Migration: a host whose loader is not kiln's own, or a tool that reads cooked KTX2 without
    Zstd support, sets `supercompression = None` (or `kiln-cook --zstd 0`). Existing named-layout
    stores keep their uncompressed files until deleted (open-questions R9); both load.
- **Breaking (cook): the default target cooks block-compressed textures.** `TargetProfile::blockFamily`
  defaults to `BC` (docs/design/bcn-encoding.md, step 5): color, UI and ORM become BC7, normals BC5,
  masks BC4/BC5, HDR BC6H. `kiln-cook --block` defaults to `bc`.
  - Every example adapter uploads BC (GL: `glCompressedTextureSubImage*`, BC1/BC3 only with S3TC;
    sokol: no BC1; NoGraphicsAPI: no BC1 or BC4), and every example shader rebuilds a normal's Z
    from X and Y, which works for BC5 and RGBA8 normal maps alike.
  - The default target's hash changed, so every store key and cooked `cookHash` changed once.
  - Migration: an adapter without BC support sets `TargetProfile::blockFamily = None` (or
    `kiln-cook --block none`). Shaders that read a normal map's Z must rebuild it from X and Y, or
    set `encoding = BC7` for normals. Delete existing stores once: a named-layout store file is
    used while it exists and is not re-cooked for a target change (open-questions R9).
- **Breaking (runtime, adapter):** `upload_status(user, token, Status* failure)`: with `Failed` the
  adapter writes why (for example `OutOfMemory` for a full pool, `Unsupported` for a resource it
  cannot make); the reason reaches K5004 / K5010 / K5009 and the Failed event instead of
  `Unknown`. New optional `Adapter::discard_upload(user, token)`: kiln calls it instead of
  `commit_upload` when reading or decoding failed after `begin_upload`, and the adapter frees the
  reservation and the object at once (kiln neither polls the token nor destroys the object);
  without it kiln commits as before and destroys the result. Null adapter:
  `NullAdapterStats::discards`, `null_adapter_break_targets()`; failed uploads report
  `OutOfMemory`. The example adapters implement both (`uploadsDiscarded` in `ex::AdapterStats`).
  Migration: add the `Status* failure` parameter (write it when returning `Failed`); implement
  `discard_upload` if the adapter can free an uncommitted upload cheaply.
- Adapter contract, documentation only (`adapter.h`, `adapter.md`): `Busy` means a retry can
  succeed, and an upload that can never fit returns `Unsupported`, a full object table
  `OutOfMemory`; kiln never polls a token after its first terminal status; `Failed` means no GPU
  work on the upload remains, and `destroy` takes partly made objects; `bind` reaches the frames
  recorded after the pump. The example adapters follow it: `kiln-nga` no longer returns `Busy` for
  an upload larger than its staging ring or mesh heap (the asset stayed Pending forever), and GL,
  sokol and NoGraphicsAPI fail a full object table instead of waiting on it.
- CI: every push builds the library, the tools, `kiln-headless` and the tests on the platform
  matrix, plus the shipping contract. The GPU examples, `kiln-nga`, the texture-only build,
  sanitizers and the compile-time report moved to `.github/workflows/extended.yml`, run by hand
  (Actions > extended > Run workflow), each group behind a checkbox. Fuzzing is not in CI.
- **Breaking (core):** `Status` and `Result<T>` are `[[nodiscard]]` types, so every ignored one
  warns (write `(void)` where discarding is intended, as for `diagf`). `Result`, `Vec`,
  `FixedArray` and `HashMap` require `NothrowStorable` element types: copy, move and destruction
  never throw. `texture_level_layout()` documents and handles invalid input (non-power-of-two
  alignment, more than 32 levels, sizes past u64). Migration: add `(void)` to deliberate
  discards; give stored types `noexcept` special members.
- **Breaking (runtime, adapter):** `Adapter::is_upload_complete` is now
  `UploadStatus (*upload_status)(user, token)`, returning `Pending`, `Complete` or `Failed`. An adapter
  can now fail an upload after `commit_upload` (a full pool, out of GPU memory): a first load fails
  with K5004 and keeps showing its placeholder, a reload keeps the current version (K5010), a
  placeholder fails `create()` (K5009). kiln destroys the failed object at once. Null adapter:
  `null_adapter_fail_uploads()` and `NullAdapterStats::uploadsFailed`. Migration: return
  `Complete` where `is_upload_complete` returned true and `Pending` where it returned false;
  return `Failed` where the adapter used to hand kiln an unusable object.
- **Breaking (runtime):** `gpu(ctx, handle)` is now `gpu_object(ctx, handle)`: `gpu` collided with
  the `gpu` namespace of NoGraphicsAPI (and is a likely name elsewhere). `TextureInfo::gpu` keeps its
  name. Migration: rename the calls.
- **Breaking (runtime, adapter):** kiln learns the host's frames, releases GPU objects after them
  and numbers bindless slots itself (`docs/design/adapter-frames-slots.md`).
  - `PumpOptions` gains `frame` (the frame the host records after this pump) and `completedFrame`
    (the last one the GPU finished); 0 keeps the last value. An object or slot number kiln drops
    during a pump is released once `completedFrame` reaches that pump's `frame`. A host that never
    reports gets immediate release.
  - `Adapter::destroy(user, obj)` replaces `destroy_deferred`: free now, kiln has waited for the
    frames. kiln also finishes uploads it abandons (unload or failure while in flight): it keeps
    polling the token and destroys the object after it completes.
  - `Adapter::bindlessSlots` and `Adapter::bind(user, slot, obj, shape)` replace `acquire` and
    `publish`. kiln hands out slots in `[0, bindlessSlots)`, one per texture asset, and calls
    `bind` at the request (the placeholder of its kind and shape), when Ready, after each reload
    and with the Failed checker. `gpu()` returns kiln's slot in `GpuObject::slot`. A request with
    every slot in use fails with K5004.
  - `destroy(ctx)` destroys every object at once: wait for the GPU to go idle before it.
  - Null adapter: `NullAdapterDesc::bindlessSlots` replaces `bindless`; `NullAdapterStats::binds`
    replaces `acquires` and `publishes`; `null_adapter_flush_deferred()` is gone (`destroy` frees at
    once; simulate frames with `PumpOptions`).
  - Example adapters: `vkx::adapter_retire`, `nga_adapter_retire`, `AdapterDesc::framesInFlight`,
    `NgaAdapterDesc::framesInFlight` and `AdapterStats::slotsInUse` are gone;
    `vkx::renderer_wait_frame()` returns the frame numbers to pass to `pump()`.
  - Migration: move `acquire` + `publish` into `bind` (write `obj` into slot `slot`; the adapter no
    longer needs asset ids, placeholder ids or an id → slot map), make `destroy_deferred` an
    immediate `destroy`, delete retire lists and "destroyed while uploading" handling, and pass
    `frame` / `completedFrame` to `pump()` from the host loop if the API does not keep objects alive
    for issued commands. An adapter that tracked when kiln starts using an upload (the Vulkan
    watermark) does it in `is_upload_complete`: kiln stops polling a token at its first true.
- **Breaking (runtime, adapter):** textures carry a shape: 2D, cube or array
  (`docs/design/texture-shapes.md`, step 1).
  - New `TextureShape` and `texture_shape_name()` in `kiln/adapter.h`. The adapter's `TextureDesc`
    gains `shape`; a cube still arrives as 6 layers, in the order +X, -X, +Y, -Y, +Z, -Z.
  - `Adapter::acquire` gains a `TextureShape shape` parameter.
  - New caps `kCubeTextures` and `kArrayTextures`. kiln uploads placeholders of a shape only for an
    adapter that declares it, and a request for an undeclared shape fails with K5004.
  - `RequestOptions::textureShape` (default `Tex2D`, first request wins) picks the placeholder. A
    cooked texture of another shape fails with the new K5017.
  - Placeholders exist per shape: `placeholder_asset_id(kind, shape)`, and
    `failed_placeholder_id(shape)` replaces `kFailedPlaceholderId` (now id 13 for 2D). A context
    creates up to 15 placeholder objects instead of 5.
  - Migration: add the `shape` parameter to `acquire`, and set the caps bits if the adapter can
    bind cube or array views.
- **Breaking (cook):** settings resolve in a fixed layer order (`docs/design/settings.md`,
  "Resolution layers"): built-in defaults, host settings, sidecar, inference, `CookPolicy`, then
  validation. Host settings are defaults, so a sidecar beats them; the new `CookPolicy` (a texture
  and a mesh function plus `void* user`) has the last word and may refuse an asset (new K3007).
  - `ProviderDesc::texture` / `mesh` are now `textureDefaults` / `meshDefaults`; new
    `ProviderDesc::policy`.
  - New `resolve_texture_layers` / `resolve_mesh_layers` and `ResolveDesc`, `CookAssetInfo`, used
    by the provider and `kiln-cook`. The color space and the Normal-only flags are derived after
    the policy.
  - The `kiln-cook` CLI is now `cook_cli_main(argc, argv, policy)` in `kiln/cook/cli.h`; a
    project's own cook tool calls it with its policy.
  - Migration: rename the two `ProviderDesc` fields. A rule that must hold even against a sidecar
    moves from the defaults into a `CookPolicy`.
- **Breaking (runtime, cook, store, tools):** asset names are `root:path/file.ext#sub`, the source
  path with its extension (`docs/design/asset-model-next.md`, Part 2).
  - The runtime no longer normalizes names: no extension stripping, no `./`, `\` or `//` clean-up.
    `request_*`/`register_*` check a name with the new `check_asset_name()` and reject a bad one
    at the call (null handle, new K5013). `asset_id()` hashes the name as given (0 if invalid).
    New helpers in `kiln/assets.h`: `check_root_name`, `split_asset_name`, `resolve_asset_name`
    (a relative URI in a source to a name in the same root), `store_file_path`.
  - `ContextDesc::sourceRoots` is now `ContextDesc::roots` (`Root{name, dir}`; an empty name is
    the default root), and `source_roots()` is `roots()`. `create()` rejects a bad or repeated
    root name (K5013).
  - Store files are `<store>/<name>.mesh|.ktx2` (`props/chair.glb.mesh`,
    `props/chair.glb#wood.ktx2`); a named root `m:` is the top-level directory `@m/`, so a
    default-root path may not start with `@` (K5013).
  - The provider finds `<root dir>/<path>` with no extension search. The extension gives the
    kind; a mismatch is K5014, an unknown root K5015, a name that differs in case from the file
    on disk K5016 (checked on Windows, `cook::source_case_matches`).
  - Mesh cook: `MeshSource::assetPath` must be a valid name. An external URI that is absolute or
    leaves the root is the new error K1020. K1019 now checks `<mesh>#<image>` with the name rules,
    which also reject `< > " | ? *`.
  - `kiln-cook`: names keep the extension; `--root [<name>=]<dir>` is repeatable and sets the
    default root when it has no name. `kiln-viewer` takes mesh names (`Lantern.glb`,
    `lib:props/chair.glb`) instead of `.mesh` store paths. `kiln-viewer` and `kiln-headless` take
    `--root` too and keep `--source <dir>` for the default root. `kiln-headless` arguments are a
    name plus `.mesh`/`.ktx2` (`Lantern.glb.mesh`). `tools/cli.h` options gain an `each` callback for repeatable options.
  - Migration: delete the store and re-cook. Request `chair.glb` instead of `chair` and
    `chair.glb#wood` instead of `chair#wood`. Replace `sourceRoots = {dir}` with
    `roots = {Root{{}, dir}}`. Resolve external texture URIs with `resolve_asset_name()`. A
    store cooked elsewhere keeps working under names without a source extension.
- **Breaking (format, cook, store):** a mesh cook no longer cooks the images its glTF references by
  URI (`docs/design/asset-model-next.md`, "one source file per cooked asset").
  - `.mesh` 0.4: `TextureBinding` flag `kTextureExternal` marks such a binding; `pathStr` is the
    URI (percent-decoded, relative to the source) and `textureId` is 0. The host maps it to a
    texture (the viewer resolves it against the mesh's directory).
  - Embedded images are named `<mesh>#<image name>` (was `<mesh>/<sanitized stem>`); an unnamed
    image is `image<N>`. A duplicate or reserved-character name is the new error K1019.
  - `TextureRef` lists embedded images only and loses `uri`. The provider splits a texture
    request at `#` instead of guessing the owning mesh from the last `/`.
  - `kCookerVersion` 3.
  - Migration: delete the store and re-cook; 0.3 files fail with `VersionMismatch`. Request
    externally referenced textures by their own names; the name rules pick their usage.
- Tests: `kiln_tests` compiles in the corpus, golden and scratch (`<build>/tests/samples`)
  directories, so running it by hand runs every test; `--corpus`, `--golden` and `--samples` still
  override. A missing directory now stops the run (exit 2) instead of silently skipping tests.
- **Breaking (cook):** `cook_mesh` and `cook_texture` take `CookEnv const& env = {}` in place of the
  trailing `Allocator const* alloc, DiagSink const* diag`. Migration:
  `cook_texture(src, s, target, alloc, &sink)` becomes `cook_texture(src, s, target, {.alloc = alloc, .diag = &sink})`;
  `cook_mesh(src, s, target, default_allocator())` becomes `cook_mesh(src, s, target)`.
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
