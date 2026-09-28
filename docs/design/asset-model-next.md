# Asset model (next)

Status: **Draft, in discussion** (2026-09-28). Nothing here is implemented. It records decisions
taken so far in an owner discussion, topic by topic. A retrospective against the original intent
follows before any code changes.

## Why

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

**B3. There is no separate ad-hoc mode.** *Decided.* Viewing a random `.glb` means a project whose
default mount is the folder given as the source. If its references leave that folder, pick a
higher folder or the cook fails.

**B4. Guarantees hold only for input that follows the rules.** *Decided in principle.* Input that breaks
a rule gets a diagnostic and never a crash or a partial store write. Stable identities, sharing,
dependency tracking and change reports are promised only for input that follows the rules.
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

**I4. Sub-assets.** *Decided.*

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
re-cooked.

**I7. Identity is the path; no GUIDs.** *Decided for now.* Moving or renaming a source breaks
references, as it already does for glTF URIs. GUID sidecars (Unity-style `.meta`) survive moves
but double the file count and weaken "sources are the truth". Worth exploring later.

## Consequences for the current code

Not a plan yet; listed so the retrospective can weigh them.

- Texture names change from `<mesh>/<stem>` to the image's own identity (external) or
  `model.ext#name` (embedded). Stores need a re-cook; the `.mesh` format changes (I6).
- The provider's extension loop and owner guess go away.
- `ContextDesc::sourceRoots` becomes a list of named mounts.
- `TextureRef::assetPath` in `include/kiln/cook/cook.h` changes meaning.

## Later topics

In the agreed order, after the retrospective:

1. **Inclusion.** How an asset enters the project: discovered on request (today's cook-on-miss),
   an import step, or a manifest.
2. **Dependency graph.** Per asset, the source files it read and its settings; the reverse
   direction answers "what does this file affect". Sharing, back-references and invalidation
   come from it.
3. **Store layout.** Mapping identities to cooked files: named, flat, or content-addressed. See
   `open-questions.md` R4 (index file).
4. **Update rules.** When a cooked file is stale (source hash, settings hash, cooker version)
   and who re-cooks it.
5. **Hot reload as a report plus a plan.** The asset layer detects changes and reports the
   affected assets. The engine decides what to reload, when and how much, and calls
   `request_reload`. Automatic reload (`hot-reload.md`) is paused until this is designed. Open:
   mark `watchStore`/`watchSources` experimental or remove them.

Also deferred: overlays (B1), the material library file (B2), GUIDs (I7), the concrete
guarantee list (B4).
