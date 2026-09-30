# Asset model (next)

Status: **Draft, experimental** (2026-09-28, branch `exp/single-file-scope`). Parts 1 and 2
record an owner discussion; the retrospective below narrows its scope and marks what it
supersedes. "Implementation" below lists what is built and the choices made on the way.

## Retrospective: scope cut back to single files

*Decided (owner, 2026-09-28).*

The original intent was asynchronous loading and cooking, mainly of textures, which have no
dependencies between them. Meshes were in the plan from M2. The trouble came later, when the cook
began to resolve a mesh's texture references itself: it cooks the images as a side effect, names
them, guesses their owner on cook-on-miss, and records them for hot reload. That makes kiln own a
dependency graph, and sharing, back-references, update rules and invalidation follow from it.
That is an asset database, and an asset database is project policy: some projects have no
authored meshes, some have their own material system or editor database.

**Rule: one source file produces one cooked asset. kiln never follows a reference.**

- Textures: unchanged.
- Meshes stay as leaf assets. A `.mesh` records its material names and texture references
  (URI as written, slot, UV set) as data. kiln does not cook, name, load or track them.
- The host resolves references: it reads them from the mesh view and requests the textures it
  wants, under names it chooses. This extends open question #6 (material remapping is the
  host's job) to textures.
- Hot reload is one file to one asset. Whether a mesh reload also reloads textures is the
  host's decision.
- Async loading, the upload budget, placeholders, load groups and the store do not change.

**Embedded images are the one exception** (a `.glb` holds several images). *Decided (owner,
2026-09-28): option (c).*

- (b) An explicit `kiln-cook` step extracts them to files, which are then normal texture sources.
  The one-to-one rule has no exception.
- (c) The mesh cook writes them as extra outputs named `model.glb#name`. kiln tracks nothing
  beyond that. The viewer's "drop in a glb and see it" keeps working.
- (a), not supporting them at all, was rejected: exported `.glb` files embed images by default.

**Texture usage without a mesh.** *Decided (owner, 2026-09-28).* The glTF slot used to say what
an external texture is (color, normal, ORM). A standalone texture needs another source, from
most to least specific:

1. A per-asset sidecar `<file>.<ext>.kiln` (e.g. `wood_n.png.kiln`). Its syntax is a strict
   subset of TOML: `key = value`, `#` comments, `[section]` / `[a.b]` headers, string, integer,
   float and boolean values. kiln parses it with its own small parser, with no dependency.
   Anything outside the subset, and any unknown key, is an error with file and line.
2. Name rules: a file-stem suffix (`_n`, `_normal` → normal; `_orm`, `_arm` → ORM; …).
   *Implemented* as `NameRule` / `kDefaultNameRules` in `kiln/cook/settings.h`, applied by the
   provider (`ProviderDesc::nameRules`) and `kiln-cook` to standalone texture sources.
3. Default: color, sRGB.

A sidecar and an explicit host usage win over the name rules; a `CookPolicy` wins over
everything. The full order is the layer table in `settings.md`. Proposed: warn when a host requests a texture with a kind that differs from the
cooked usage.

**Order of work:** name rules (done), then the scope cut with option (c) (done: `.mesh` 0.4,
`kTextureExternal`, `<mesh>#<name>`, K1019; see `mesh-format-spec.md` §5.7), then sidecars
(done: `kiln/cook/sidecar.h`, K3005/K3006; see `settings.md`, "Sidecar files"). The layer order
was then settled: host settings are defaults, a sidecar beats them, and a `CookPolicy` has the
last word (`settings.md`, "Resolution layers").
Then stage B, the extension in the name (I3, I5, I6), and stage C, named roots (B1, B2, I1, I2):
both done, see "Implementation".

**What stays from Parts 1 and 2:** identity as the exact source path with its extension, roots,
the path rules, and strict input rules. **Superseded:** anything that needs kiln to follow a
reference. Each such item below is marked *Superseded by the retrospective*.

## Implementation (stages B and C)

*Done 2026-09-28.* API: `kiln/assets.h` ("Asset names"), `kiln/cook/provider.h`. Diagnostics:
K1019, K1020, K5013-K5016 (`diagnostics.md`).

- **Names.** `check_asset_name()` implements I1 and I2. The runtime checks a name at
  `request_*`/`register_*` (K5013) and compares it byte for byte; `AssetId` is FNV-1a 64 of the
  whole name (I6). There is no normalization any more.
- **Roots.** `ContextDesc::roots` replaces `sourceRoots` (`Root{name, dir}`, empty name =
  the default root). `create()` checks the names. `kiln-cook`, `kiln-viewer`, `kiln-headless`: `--root [n=]dir`.
- **Provider.** The source of `m:path` is `<dir of m>/path`. The extension gives the kind (I5),
  K5014 on a mismatch; an unknown root is K5015. On Windows, a name that differs in case from
  the file on disk is K5016 (I2); elsewhere the file system already fails it.
- **References (B2).** The mesh cook fails with K1020 when a buffer or image URI is absolute or
  leaves the source's root. `resolve_asset_name()` gives hosts the same resolution, and
  `texture_asset_name()` applies it to a `.mesh` texture binding (an embedded image's name as stored,
  an external URI resolved); every example uses it.

Choices made during implementation, *Proposed* until the owner signs off:

- **Store layout of a named root:** *decided (owner, 2026-09-28); the named layout was removed
  2026-09-30 (names now live in the catalog, `store-catalog.md`), the `@` rule stays.* `m:` becomes
  the top-level directory `@m/` (`<store>/@pool/tex/wood.png.ktx2`). So that no default-root name maps to the
  same file, a path without a root prefix may not start with `@` (K5013). The first layout, `m#/`,
  needed no rule but read oddly and needs escaping in URLs.
- **More reserved characters:** names also exclude `< > " | ? *`, which Windows does not allow in
  file names. A name that works on Linux then works on Windows too, as with the case rule.
  I2 said "the store layout escapes as needed"; rejecting is simpler and stricter.
- **The kind check is in the provider, not at the call.** The runtime accepts any valid name, with
  or without a source extension: a store cooked elsewhere, or `register_*` content, has no source.
  So `request_mesh("x.png")` fails when the provider runs (K5014), not at the call.
- **Roots stay on `ContextDesc`**, as "Consequences" said, although B5 says the runtime does not
  know roots. The runtime only stores them for the provider; moving them to `ProviderDesc` is a
  small change if B5 should win.
- **Case check on Windows only.** On macOS (case-insensitive by default) a wrong-case name still
  cooks.
- **`kiln-headless`** keeps its argument form: a name plus `.mesh` or `.ktx2` for the kind.

## Why (the original symptoms)

The cook and the runtime derive everything from asset names on demand. There is no record of
which source produces which asset. Symptoms seen so far:

- Cooked texture names are `<mesh>/<image stem>`, so two models that use one external image
  cook, store, upload and bind it twice.
- The cook-on-miss provider guesses a texture's owner: it tries `<name>.png|.jpg|…`, then strips
  the last path segment and looks for a `.glb`/`.gltf` (`src/cook/provider.cpp`).
- Hot reload watches only the model file, not the external images or buffers it reads.
- The store layout mirrors the naming guess instead of being a decision of its own.
- There are no rules for what input is supported, so every combination is handled ad hoc.

## Part 1: Boundaries

**B1. A project is one or more named source roots, one store and settings.** *Decided, watch real usage.*

- A root is a directory of sources with a name. Roots have separate namespaces (`pool:`, `game:`).
- One root is the default; its names have no prefix.
- The store is a cache for one (project, target) pair.
- No project file yet: the roots given to the provider or `kiln-cook` are the project.
- Overlays (several roots in one namespace, where a later root replaces an asset, e.g. mods,
  DLC, patches) are a different dimension. They are deferred, not rejected.
- Open: whether real usage needs more than one root. The shared texture pool (B2) is the first
  case for it.

**B2. File references stay inside their root.** *Decided.*

- A glTF URI resolves relative to the glTF file and must stay inside the same root after
  normalization. A URI that leaves the root is a cook error.
- Sharing across roots goes by identity, not by path. Example: a material file
  (`material-name.toml`) names `pool:textures/wood_oak.png`. The material file is its own topic.
  *Superseded by the retrospective:* a material file is host policy; kiln does not read it.

**B3. There is no separate ad-hoc mode.** *Decided.* Viewing a random `.glb` means a project whose
default root is the folder given as the source. If its references leave that folder, pick a
higher folder or the cook fails.

**B4. Guarantees hold only for input that follows the rules.** *Decided in principle.* Input that breaks
a rule gets a diagnostic and never a crash or a partial store write. Stable identities, sharing,
dependency tracking and change reports are promised only for input that follows the rules
(*superseded by the retrospective:* sharing, dependency tracking and change reports leave kiln).
`kiln-cook --check` reports violations. To do: replace this with a concrete list of guarantees
and the diagnostic for each broken rule.

**B5. The runtime knows the store and identities.** *Direction, not a hard wall.* It does not know
roots, source paths or dependencies. Change detection and the list of affected assets live on
the cook side.

**Strictness.** *Decided.* A broken rule is an error. There are no opt-outs.

## Part 2: Identity

**I1. Syntax: `root:path/file.ext#sub`.** *Decided.*

- `pool:textures/wood_oak.png`: a file in root `pool`.
- `props/chair.glb#wood`: an image embedded in `props/chair.glb` in the default root.
- Root names are `[a-z0-9_]`, at least 2 characters, so `c:` never looks like a drive letter.

**I2. Path rules.** *Proposed with I1.*

- A path is checked once, where it enters kiln (request API, glTF URI, material file). After
  that it is compared byte for byte.
- `/` separators only, relative, no empty, `.` or `..` segments, no leading `/`.
- Any UTF-8 except control characters and `:` `#` `\`. The store layout escapes as needed.
- Case-sensitive. The cook compares the name with the exact case on disk and fails on a
  mismatch, so a name that works on Windows also works on Linux and consoles.

**I3. The extension is part of the name.** *Decided.* The name is the exact source path in its
root. Reasons:

- The provider finds the source without guessing extensions.
- Two sources can never claim one name, so no clash rule is needed.
- The owner of a sub-asset is visible in its name.
- The cost: replacing `wood.png` with `wood.jpg` changes the name. glTF URIs carry the extension
  already, so the model changes in that case anyway.

The cooked file name in the store is a separate decision (store layout).

**I4. Sub-assets.** *Decided; applies only if embedded images take option (c) of the retrospective.*

- A sub-asset lives inside another source file and has no file of its own. For now this is
  only an image embedded in a `.glb`/`.gltf`. An external image is not a sub-asset; it has its
  own identity.
- One level only: no `a#b#c`.
- The sub name is the glTF image `name`. A duplicate image name in one file is a cook error. An
  unnamed image gets `image<N>` from its index.
- A sub-asset can be requested directly. The provider splits at `#` and cooks the owner.

**I5. The kind comes from the extension.** *Proposed.* `.glb`, `.gltf`: mesh. `.png`, `.jpg`,
`.jpeg`, `.webp`, `.hdr`, `.ktx2`: texture. `#sub` of a model: texture. Requesting a mesh by a texture
name is an error at the call.

**I6. `AssetId` is FNV-1a 64 of the full identity**, root and extension included. *Proposed.*
Texture bindings in `.mesh` store full identities. This is a format break: every store must be
re-cooked. *Superseded by the retrospective:* bindings store the URI as written; the host maps it
to an identity.

**I7. Identity is the path; no GUIDs.** *Decided for now.* Moving or renaming a source breaks
references, as it already does for glTF URIs. GUID sidecars (Unity-style `.meta`) survive moves
but double the file count and weaken "sources are the truth". Worth exploring later.

## Consequences for the current code

After the retrospective. All done with stages B and C. One exception: the provider still records
the embedded images a glb wrote, because option (c) keeps them as outputs of the mesh cook.

- The mesh cook stops cooking textures. `cook_mesh_full` in `src/cook/provider.cpp` loses its
  texture half, or keeps only the embedded-image outputs of option (c).
- `TextureRef` becomes data only: URI, slot, UV set. It no longer carries an asset path for
  external images.
- The provider's extension loop, owner guess and record of emitted textures go away.
- The viewer maps texture references to names itself.
- `ContextDesc::sourceRoots` becomes a list of named roots (`ContextDesc::roots`).
- Asset names gain their extension. Stores need a re-cook.

## Later topics

The retrospective changes the list agreed before it.

- **Store layout.** Mapping identities to cooked files: named, flat, or content-addressed. See
  `open-questions.md` R4 (index file). Still needed.
- **Update rules.** When a cooked file is stale (source hash, settings hash, cooker version).
  Still needed, but only per file.
- **Hot reload.** One file to one asset. Automatic reload (`hot-reload.md`) keeps working in
  that form; the glb-to-embedded-texture relation in its source poller goes away. Open: keep
  `watchStore`/`watchSources`, or mark them experimental.
- *Superseded by the retrospective:* inclusion, the dependency graph, and hot reload as a report
  plus a plan. They are host work. A small helper may come later if real usage asks for one.

Also deferred: overlays (B1), GUIDs (I7), the concrete guarantee list (B4).
