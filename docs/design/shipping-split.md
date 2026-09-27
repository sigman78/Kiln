# Read-only shipping (M1.5)

**Status:** Decided (2026-09-27), except the mechanism choice (Decision 2), which is the
coordinator's recommendation: **Proposed (awaiting owner sign-off)**.
**Milestone:** M1.5
**Decides:** What a shipping build of a product built on kiln links and installs; the boundary
between `kiln_runtime` and `kiln_cook`; the header, install-component and test split that enforces
it; the shipping presets and the CI job that keeps the contract green.

This note turns HANDOFF §1.1 rule 3 ("shipping builds ... link only `kiln_core` + `kiln_runtime`")
into a concrete, checked contract, ahead of M2 (cooker) and M3 (runtime) landing real code.

## Decision

### 1. Shipping contract

A product that ships pre-cooked assets links **`kiln_core` + `kiln_runtime` only**.

- `kiln_runtime` never writes files, never imports source formats (glTF, PNG), never encodes, and
  has no dependency, symbol reference or link edge to `kiln_cook`.
- `kiln_runtime` reaches cooking only through a function-pointer **cook provider** table that
  `kiln_cook` registers at startup (HANDOFF §7, `kiln::cook::install_provider`). There is no symbol
  reference from runtime to cook: the provider is the only seam, and it is absent by construction
  when `kiln_cook` is not linked.
- Third-party code inside `kiln_runtime`: **none in v0.5**. Later, only decoders (zstd
  decode-only, meshoptimizer's vertex/index decoder sources, per `dependencies.md`), built from
  source with kiln's own compiler flags. No encoder, importer or third-party writer ever enters
  `kiln_runtime`.
- `kiln-info` is read-only (it dumps `.mesh` / KTX2 headers and blob tables; see `error-model.md`
  and HANDOFF M1) and may ship with tools even though it is built alongside `kiln_cook` today (see
  Open points).

### 2. Mechanism: structural split first, conditional compilation only for hot reload

**Proposed (awaiting owner sign-off).**

The split between "ships" and "dev/cook-only" is enforced by **CMake target boundaries**, not by
preprocessor conditions on a single library. `kiln_runtime` is a separate target from `kiln_cook`
today (§3 of HANDOFF, already in `CMakeLists.txt`); this note fixes that as the permanent shape of
the boundary rather than an implementation detail that could later collapse into one library with
an `ifdef`.

**Exception: hot reload.** The file watcher and the re-cook hook stay inside `kiln_runtime`,
behind `KILN_HOT_RELOAD` (already a CMake option, HANDOFF §9). Reasons this one feature is
conditional compilation, not a target split:

- Reloading a pre-cooked file that an external `kiln-cook` process rewrote (asset pipeline running
  next to a shipping-shaped binary, or a live-service title patching assets on disk) is useful
  **without** the cooker linked in. Splitting hot reload into its own target would not track the
  cook/no-cook line; it tracks its own, independent axis (watch files, yes or no).
- It is a small, self-contained feature (watcher, dependency propagation, generation swap) with no
  third-party code and no import/encode logic, so an `ifdef` does not reopen the ifdef-hygiene
  problem this note otherwise avoids.
- Shipping presets set `KILN_HOT_RELOAD=OFF` (Decision 4), so it never ships regardless.

The dev-only magenta "failed" placeholder (`handles-and-states.md`) is **not** part of this split.
It is a runtime flag, `ContextDesc.devPlaceholders` (default `KILN_DEBUG != 0`), because it changes
only which texels a placeholder uploads, never what is linked or compiled in.

### 3. Header boundary

Cook-only public headers live under `include/kiln/cook/`:

| Header | Today |
|---|---|
| `include/kiln/cook/mesh_writer.h` | exists (M1) |
| `include/kiln/cook/ktx2_writer.h` | exists (M1) |
| `include/kiln/cook/settings.h` | later (M2, §5) |
| `include/kiln/cook/cook.h` | later (M2, §6.2) |
| `include/kiln/cook/*` importer headers | later (M2) |

- Shipping installs only `include/kiln/*.h` (the flat, runtime-and-core headers), never
  `include/kiln/cook/`.
- Including a cook header from shipping code fails at compile time: the header is not on disk in a
  shipping install, and `kiln_runtime` does not add `include/kiln/cook/` to its include path even
  in a source build. There is no runtime check to get this wrong quietly.

### 4. Shipping presets and CI

Four new CMake presets, one per compiler/OS pair already in `CMakePresets.json`:

| Preset | Base | `KILN_BUILD_COOK` | `KILN_HOT_RELOAD` | `KILN_BUILD_TOOLS` | Tests |
|---|---|---|---|---|---|
| `win-msvc-shipping` | Release | OFF | OFF | OFF | ON |
| `win-clangcl-shipping` | Release | OFF | OFF | OFF | ON |
| `linux-clang-shipping` | Release | OFF | OFF | OFF | ON |
| `linux-gcc-shipping` | Release | OFF | OFF | OFF | ON |

A new CI job, `shipping`, builds one Windows shipping preset and one Linux shipping preset, then:

1. Runs the reader-only test suite (Decision 6) against that build.
2. Installs the build to a prefix (`cmake --install`).
3. Configures, builds and runs `tests/shipping_consumer`, a tiny program that does nothing but
   `find_package(kiln CONFIG REQUIRED)` and `target_link_libraries(app PRIVATE kiln::runtime)`,
   against that install prefix.

This continuously proves the HANDOFF §11.3 exit criterion ("a shipping-config build links only
`kiln_core` + `kiln_runtime` and loads from a pre-cooked store") instead of relying on someone
remembering to check it once at the v0.5 tag.

### 5. Install components

```
find_package(kiln CONFIG REQUIRED)
# -> kiln::core, kiln::runtime

find_package(kiln CONFIG REQUIRED COMPONENTS cook)
# -> also kiln::cook; fails clearly if cook was not built/installed
```

- Export sets: `kilnRuntimeTargets` (core + runtime) and `kilnCookTargets` (cook), installed
  separately so a shipping install genuinely does not carry cook's export file, not merely omit
  linking it.
- `find_package(kiln COMPONENTS cook)` against an install produced with `KILN_BUILD_COOK=OFF` (any
  shipping preset) fails with a clear "component cook not found" message, not a silent partial
  configure.

### 6. Tests split

| Test file | Includes `kiln/cook/`? | Builds when |
|---|---|---|
| `test_mesh_read.cpp` | never | always (reader-only) |
| `test_ktx2_read.cpp` | never | always (reader-only) |
| `test_formats.cpp` | never | always (reader-only) |
| core tests (`test_core.cpp`, `test_alloc.cpp`, `test_containers.cpp`, `test_hash.cpp`, `test_result.cpp`) | never | always |
| `test_mesh.cpp` (writer tests) | yes | only when `kiln_cook` is built |
| `test_ktx2.cpp` (writer tests) | yes | only when `kiln_cook` is built |
| `kiln-info` sample tests | yes (via cooked fixtures) | only when `kiln_cook` is built |

- Reader tests use hand-built byte images (already the M1 approach) and, from M2, checked-in
  golden files, so they never need a cooker to produce their own fixtures.
- This is the file layout the reader-only test suite in the `shipping` CI job (Decision 4) runs:
  everything in the left column above, nothing that needs `kiln_cook`.
- This note records the target shape. The actual file split (today `test_mesh.cpp` and
  `test_ktx2.cpp` hold both read and write cases) happens as tests land in M2, not as a rename of
  existing M1 files under this note.

### 7. Cache-less mode and cook-on-miss are dev-only by construction

Both need a registered cook provider (§6.3, §4.3 of HANDOFF). Neither is reachable in a shipping
build, because `kiln_cook` is not linked and nothing else can call
`kiln::cook::install_provider`. No flag disables them; they are simply unreachable. A shipping
build's store (HANDOFF §4.4) is a plain read-only directory, exactly as HANDOFF already states:
"a host that cooks everything in its build step can treat it as a plain read-only directory."

## Rationale

Structural split vs conditional compilation, compared on the axes that matter for a shipping
contract:

| | Structural split (target boundary) | Conditional compilation (`KILN_NO_COOK` ifdef) |
|---|---|---|
| **Enforcement** | The linker enforces it. Cook code cannot be reached from runtime because it is not in the same translation unit set, let alone the same binary. | Relies on every contributor remembering the ifdef and on nobody adding an unguarded include. Discipline, not a mechanical guarantee. |
| **Binary identity** | The runtime binary a dev build produces and the one a shipping build produces come from the same object files; only what else is linked differs. | Two different sets of object files (`#ifdef`'d in vs out) mean dev and ship are never quite the same binary, which weakens "we tested the dev build" as evidence for shipping. |
| **Consumer visibility** | The boundary is visible in target names (`kiln::runtime` vs `kiln::cook`) and in `find_package` components. A consumer's own `CMakeLists.txt` documents what it ships. | Invisible from outside; a consumer has to know which macro was on when the library was built. |
| **Cost of adding a feature** | A new cook-only feature: add a file to `kiln_cook`, done. A new runtime feature: add a file to `kiln_runtime`, done. No ifdef to thread through call sites. | Every cook-reachable code path needs an `#ifdef KILN_NO_COOK` (or equivalent) at each call site that might exist in a runtime-only build, and it grows with every feature. |

The one exception, hot reload behind `KILN_HOT_RELOAD` (Decision 2), does not fight this: it is a
single feature, self-contained, already an option in `CMakeLists.txt`, and orthogonal to the
cook/no-cook line (a shipping-shaped binary may reasonably want it on, in principle, even though
the shipping presets in this note turn it off).

## Alternatives considered

| Alternative | Why not |
|---|---|
| Single library with `KILN_NO_COOK` ifdefs | No linker enforcement; dev and shipping binaries diverge only by macro, which is easy to get wrong quietly; the boundary is invisible to a consumer's build. |
| Separate repositories for runtime and cook | Enforces the boundary even harder, but breaks the single-source-of-truth for shared types (`Handle`, `Result`, `.mesh`/KTX2 record layouts) that both sides read, and complicates the M0-era single-repo workflow (one CI, one version, one `CHANGELOG.md`) for no benefit the target split doesn't already give. |
| `kiln_runtime` as header-only | Would remove the `.cpp` boundary that currently keeps third-party headers and heavy std headers out of `include/kiln/` (HANDOFF §2 header hygiene); also reintroduces the "did I accidentally pull in a cook header" risk at every consumer's compile, instead of once at kiln's own build. |

## Consequences / what M2+ must respect

- **Every new dependency lands in `kiln_cook` unless it is a decoder.** `dependencies.md`'s
  runtime-side dependency rule (decoders only, built from source, our flags) is the only door into
  `kiln_runtime`. An encoder, importer, or anything that writes files is a cook dependency by
  default; adding it to `kiln_runtime` needs an explicit exception recorded in `dependencies.md`
  and this note.
- **Every new public header declares its side.** A header goes in `include/kiln/` if
  `kiln_runtime` (or `kiln_core`) needs it to compile or if shipping code needs it to consume
  runtime data; it goes in `include/kiln/cook/` if only `kiln_cook` (or a tool that links it)
  needs it. There is no third location and no header that is "mostly runtime, but also declares
  one cook function".
- **CI gate.** The `shipping` job (Decision 4) is required, not advisory: a change that makes
  `kiln_runtime` fail to build with `KILN_BUILD_COOK=OFF`, or that makes `tests/shipping_consumer`
  fail to configure, build or run, fails CI the same as a broken unit test.
- Install components (Decision 5) mean a consumer's own `find_package(kiln COMPONENTS cook)` is
  the health check for "did my build actually ship cook by mistake"; M2+ work should keep that
  failure message clear rather than a generic CMake configure error.

## Open points for the owner

- Whether `kiln-info` ships in product (today it links against cook-produced fixtures for its own
  tests, per Decision 6, but the tool itself is read-only per Decision 1). Proposed: it may ship,
  since it never writes and has no cook dependency at link time; needs an explicit decision once
  tools packaging is designed.
- Whether hot reload should move out of `kiln_runtime` into a separate `kiln_devtools` target
  later, once it grows (native watchers post-v0.9, per HANDOFF §11.4) past "a few files behind a
  flag". Not needed for v0.5; noted here so the option isn't foreclosed by how M1.5 ships.
