# Asset model (next)

Status: **Draft, experimental** (2026-09-28, branch `exp/single-file-scope`). Nothing here is
implemented. Parts 1 and 2 record an owner discussion; the retrospective below narrows its scope
and marks what it supersedes.

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

**Embedded images are the one exception** (a `.glb` holds several images). *Open, owner deciding:*

- (b) An explicit `kiln-cook` step extracts them to files, which are then normal texture sources.
  The one-to-one rule has no exception.
- (c) The mesh cook writes them as extra outputs named `model.glb#name`. kiln tracks nothing
  beyond that. The viewer's "drop in a glb and see it" keeps working.
- (a), not supporting them at all, was rejected: exported `.glb` files embed images by default.

**What stays from Parts 1 and 2:** identity as the exact source path with its extension, mounts,
the path rules, and strict input rules. **Superseded:** anything that needs kiln to follow a
reference. Each such item below is marked *Superseded by the retrospective*.

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

**B1. A project is one or more mounted source roots, one store and settings.** *Decided, watch real usage.*

- A mount is a source root with a name. Mounts have separate namespaces (`pool:`, `game:`).
- One mount is the default; its names have no prefix.
- The store is a cache for one (project, target) pair.
- No project file yet: the mounts given to the provider or `kiln-cook` are the project.
- Overlays (several roots in one namespace, where a later root replaces an asset, e.g. mods,
  DLC, patches) are a different dimension. They are deferred, not rejected.
- Open: whether real usage needs more than one mount. The shared texture pool (B2) is the first
  case for it.

**B2. File references stay inside their mount.** *Decided.*

- A glTF URI resolves relative to the glTF file and must stay inside the same mount after
  normalization. A URI that leaves the mount is a cook error.
- Sharing across mounts goes by identity, not by path. Example: a material file
  (`material-name.toml`) names `pool:textures/wood_oak.png`. The material file is its own topic.
  *Superseded by the retrospective:* a material file is host policy; kiln does not read it.

**B3. There is no separate ad-hoc mode.** *Decided.* Viewing a random `.glb` means a project whose
default mount is the folder given as the source. If its references leave that folder, pick a
higher folder or the cook fails.

**B4. Guarantees hold only for input that follows the rules.** *Decided in principle.* Input that breaks
a rule gets a diagnostic and never a crash or a partial store write. Stable identities, sharing,
dependency tracking and change reports are promised only for input that follows the rules
(*superseded by the retrospective:* sharing, dependency tracking and change reports leave kiln).
`kiln-cook --check` reports violations. To do: replace this with a concrete list of guarantees
and the diagnostic for each broken rule.

**B5. The runtime knows the store and identities.** *Direction, not a hard wall.* It does not know
mounts, source paths or dependencies. Change detection and the list of affected assets live on
the cook side.

**Strictness.** *Decided.* A broken rule is an error. There are no opt-outs.

## Part 2: Identity

**I1. Syntax: `mount:path/file.ext#sub`.** *Decided.*

- `pool:textures/wood_oak.png`: a file in mount `pool`.
- `props/chair.glb#wood`: an image embedded in `props/chair.glb` in the default mount.
- Mount names are `[a-z0-9_]`, at least 2 characters, so `c:` never looks like a drive letter.

**I2. Path rules.** *Proposed with I1.*

- A path is checked once, where it enters kiln (request API, glTF URI, material file). After
  that it is compared byte for byte.
- `/` separators only, relative, no empty, `.` or `..` segments, no leading `/`.
- Any UTF-8 except control characters and `:` `#` `\`. The store layout escapes as needed.
- Case-sensitive. The cook compares the name with the exact case on disk and fails on a
  mismatch, so a name that works on Windows also works on Linux and consoles.

**I3. The extension is part of the name.** *Decided.* The name is the exact source path in its
mount. Reasons:

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
`.jpeg`, `.webp`, `.ktx2`: texture. `#sub` of a model: texture. Requesting a mesh by a texture
name is an error at the call.

**I6. `AssetId` is FNV-1a 64 of the full identity**, mount and extension included. *Proposed.*
Texture bindings in `.mesh` store full identities. This is a format break: every store must be
re-cooked. *Superseded by the retrospective:* bindings store the URI as written; the host maps it
to an identity.

**I7. Identity is the path; no GUIDs.** *Decided for now.* Moving or renaming a source breaks
references, as it already does for glTF URIs. GUID sidecars (Unity-style `.meta`) survive moves
but double the file count and weaken "sources are the truth". Worth exploring later.

## Consequences for the current code

Not a plan yet. After the retrospective:

- The mesh cook stops cooking textures. `cook_mesh_full` in `src/cook/provider.cpp` loses its
  texture half, or keeps only the embedded-image outputs of option (c).
- `TextureRef` becomes data only: URI, slot, UV set. It no longer carries an asset path for
  external images.
- The provider's extension loop, owner guess and record of emitted textures go away.
- The viewer maps texture references to names itself.
- `ContextDesc::sourceRoots` becomes a list of named mounts.
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
