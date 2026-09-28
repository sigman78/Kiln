# Read-only shipping (M1.5)

**Status:** Decided (2026-09-27), except the mechanism choice (Decision 2): **Proposed (awaiting
owner sign-off)**. Implemented: `CMakeLists.txt` (targets, install), `CMakePresets.json`
(`*-shipping`), `tests/CMakeLists.txt`, `tests/shipping_consumer/`, the `shipping` job in
`.github/workflows/ci.yml`.
**Decides:** What a shipping build of a product built on kiln links and installs; the boundary
between `kiln_runtime` and `kiln_cook`; the header, install-component and test split that enforces
it; the shipping presets and the CI job that keeps the contract green.

## Decision

### 1. Shipping contract

A product that ships pre-cooked assets links **`kiln_core` + `kiln_runtime` only** (HANDOFF rule).

- `kiln_runtime` never writes files, never imports source formats (glTF, PNG, JPEG, WebP), never encodes, and
  has no dependency, symbol reference or link edge to `kiln_cook`.
- `kiln_runtime` reaches cooking only through the `CookProvider` function-pointer table that
  `kiln::cook::install_provider` registers. The provider is the only seam, and it is absent by
  construction when `kiln_cook` is not linked.
- Third-party code inside `kiln_runtime`: **none in v0.5**. Later, only decoders (zstd
  decode-only, meshoptimizer's vertex/index decoder sources, per `dependencies.md`), built from
  source with kiln's own flags.
- `kiln-info` is read-only and links only `kiln_runtime`. It installs with the runtime, but the
  shipping presets do not build tools.

### 2. Mechanism: structural split first, conditional compilation only for hot reload

**Proposed (awaiting owner sign-off).**

The split between "ships" and "dev/cook-only" is enforced by **CMake target boundaries**, not by
preprocessor conditions on a single library. `kiln_runtime` and `kiln_cook` are separate targets,
and this note fixes that as the permanent shape of the boundary.

**Exception: hot reload** (M5, not implemented). The file watcher and the re-cook hook stay inside
`kiln_runtime`, behind the `KILN_HOT_RELOAD` CMake option (today it only defines
`KILN_HOT_RELOAD=1`). Reasons:

- Reloading a pre-cooked file that an external `kiln-cook` rewrote is useful **without** the cooker
  linked in. Hot reload tracks its own axis (watch files, yes or no), not the cook/no-cook line.
- It is small and self-contained (watcher, dependency propagation, version swap) with no
  third-party code and no import/encode logic.
- Shipping presets set `KILN_HOT_RELOAD=OFF`, so it never ships.

The magenta "failed" placeholder is **not** part of this split. It is a runtime flag,
`ContextDesc.devPlaceholders` (default `KILN_DEBUG != 0`), because it changes only which texels a
placeholder uploads.

### 3. Header boundary

Cook-only public headers live under `include/kiln/cook/`: `mesh_writer.h`, `ktx2_writer.h`,
`settings.h`, `cook.h`, `image.h`, `provider.h`. Everything else in `include/kiln/` is core or
runtime.

- A shipping install carries only `include/kiln/*.h`. The `include/kiln/cook/` directory installs
  with the `cook` component, which exists only when `kiln_cook` is built.
- In a shipping install, including a cook header fails at compile time: the file is not there. In
  a source tree the cook headers sit under the same include root as the rest, so there the guard
  is the `shipping` CI job, which builds everything without `kiln_cook`.

### 4. Shipping presets and CI

One preset per compiler/OS pair, all Release:

| Preset | `KILN_BUILD_COOK` | `KILN_HOT_RELOAD` | `KILN_BUILD_TOOLS` | Tests, examples, viewer |
|---|---|---|---|---|
| `win-msvc-shipping` | OFF | OFF | OFF | ON |
| `win-clangcl-shipping` | OFF | OFF | OFF | ON |
| `linux-clang-shipping` | OFF | OFF | OFF | ON |
| `linux-gcc-shipping` | OFF | OFF | OFF | ON |

`KILN_BUILD_EXAMPLES` and `KILN_BUILD_VIEWER` are ON in every preset, so the shipping presets also
build `kiln-headless`, `kiln-viewer` and `kiln-vk-smoke` without the cook side.

The CI job `shipping` builds `linux-gcc-shipping` and `win-msvc-shipping`, then:

1. Runs the reader-only test suite (Decision 6).
2. Installs to a prefix and checks that no cook header was installed.
3. Configures, builds and runs `tests/shipping_consumer`, which only does
   `find_package(kiln CONFIG REQUIRED)` and links `kiln::runtime`, against that prefix.

This continuously proves the v0.5 exit criterion "a shipping-config build links only `kiln_core` +
`kiln_runtime` and loads from a pre-cooked store".

### 5. Install components

```
find_package(kiln CONFIG REQUIRED)                  # kiln::core, kiln::runtime
find_package(kiln CONFIG REQUIRED COMPONENTS cook)  # also kiln::cook
```

- Export sets: `kilnRuntimeTargets` (core + runtime) and `kilnCookTargets` (cook, component
  `cook`), installed separately, so a shipping install does not carry cook's export file.
- `find_package(kiln COMPONENTS cook)` against an install without cook fails with a message
  that names the missing `cook` component, not a silent partial configure.

### 6. Tests split

`kiln_tests` always builds the core, reader, IO, null adapter and runtime tests (`test_core`,
`test_alloc`, `test_result`, `test_hash`, `test_containers`, `test_formats`, `test_mesh_read`,
`test_ktx2_read`, `test_ktx2_corpus`, `test_null_adapter`, `test_io`, `test_runtime`). The writer,
cooker, settings, store, provider, golden and `kiln-info` tests join only when `kiln_cook` is built.

- Reader tests use hand-built byte images, the KTX2 corpus and committed golden files, so they
  never need a cooker to produce their fixtures.
- `test_mesh.cpp` and `test_ktx2.cpp` hold writer round trips and are cook-only.

### 7. Cache-less mode and cook-on-miss are dev-only by construction

Both need a registered cook provider. Neither is reachable in a shipping build, because
`kiln_cook` is not linked and nothing else can call `install_provider`. No flag disables them. A
shipping build's store is a plain read-only directory.

## Rationale

| | Structural split (target boundary) | Conditional compilation (`KILN_NO_COOK` ifdef) |
|---|---|---|
| **Enforcement** | The linker enforces it. | Relies on every contributor remembering the ifdef. |
| **Binary identity** | Dev and shipping runtimes come from the same object files; only what else is linked differs. | Two sets of object files, so testing the dev build is weaker evidence for shipping. |
| **Consumer visibility** | Visible in target names (`kiln::runtime` vs `kiln::cook`) and `find_package` components. | Invisible; a consumer must know which macro was on. |
| **Cost of a feature** | Add a file to the right target. | Every cook-reachable call site needs an `#ifdef`. |

Hot reload behind `KILN_HOT_RELOAD` does not fight this: it is one self-contained feature,
orthogonal to the cook/no-cook line.

## Alternatives considered

| Alternative | Why not |
|---|---|
| Single library with `KILN_NO_COOK` ifdefs | No linker enforcement; the boundary is invisible to a consumer's build. |
| Separate repositories for runtime and cook | Breaks the single source of truth for shared types and record layouts, and the single-repo workflow, for no benefit the target split does not already give. |
| `kiln_runtime` as header-only | Removes the `.cpp` boundary that keeps third-party and heavy std headers out of `include/kiln/`. |

## Consequences / what later work must respect

- **Every new dependency lands in `kiln_cook` unless it is a decoder.** Adding anything else to
  `kiln_runtime` needs an explicit exception recorded in `dependencies.md` and this note.
- **Every new public header declares its side:** `include/kiln/` if `kiln_runtime` or `kiln_core`
  needs it or shipping code consumes it, `include/kiln/cook/` if only `kiln_cook` (or a tool that
  links it) needs it. No third location, no mixed header.
- **CI gate.** The `shipping` job is required, not advisory.
- A consumer's own `find_package(kiln COMPONENTS cook)` is the health check for "did my build ship
  cook by mistake"; keep its failure message clear.

## Open points for the owner

- Whether `kiln-info` ships in products. Proposed: it may ship, since it never writes and links
  only `kiln_runtime`; decide once tools packaging is designed.
- Whether hot reload should move into a separate `kiln_devtools` target once it grows (native
  watchers post-v0.9). Not needed for v0.5.
