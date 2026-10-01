# Project config (`kiln.toml`)

**Status:** Decided (owner, 2026-10-01; decisions at the end). Implemented 2026-10-01 in rollout steps 1
to 6: `include/kiln/cook/project.h`, `src/cook/project.cpp`, `src/cook/toml_subset.cpp`; choices made
while implementing are open-questions R15.
File format: TOML (owner, 2026-10-01; open question 7).
**Decides:** the project file, its syntax and parser, where its layers sit in the resolution order,
how it reaches `kiln-cook` and the cook provider, how a change to it reaches the store and hot
reload, and `kiln-cook --explain`. The full settings model it fills in is `../cook-settings.md`;
the structs and the decided layer order are `settings.md`.

## Goal

Today a project sets cook settings in three places: host structs or `kiln-cook` flags for the
whole project, a sidecar per source file, and `CookPolicy` code. Nothing sits between "all assets"
and "one file", so "every UI texture uncompressed, no mips" needs a sidecar per file or a policy.
Embedded images have no sidecar at all.

The project file adds that middle: defaults, named presets and path rules in one file that both
`kiln-cook` and the cook provider read. `--explain` shows which layer set each field.

Non-goals: no runtime change (the file is cook-only, like sidecars); no scripting; the usage →
format table stays code (`target-profiles.md` §1).

## Example

```toml
# kiln.toml
[roots]                            # kiln-cook only; --root wins
default = "assets"                 # the default root: names without "root:"
mods    = "../mods"                # named root mods: ("mods:hud/icon.png")

[project]
store  = "build/store"             # kiln-cook only; --store wins
target = "desktop"                 # kiln-cook only; --target wins

[texture]                          # project defaults
quality = "high"

[mesh]
compression = "meshopt-zstd"

[texture.preset.ui]
usage   = "ui"
genMips = false

[texture.preset.foliage]
usage       = "color"
alphaCutoff = 0.5

[[texture.rule]]
match  = ["ui/**", "mods:hud/**"]
preset = "ui"

[[texture.rule]]
match   = ["env/trees/**", "props/*.glb#*leaf*"]   # embedded images match too
preset  = "foliage"
maxSize = 2048

[[texture.rule]]
match   = ["env/sky/**"]
targets = ["compat"]              # only when cooking for compat
maxSize = 2048


[texture.usage.normal]             # after inference, for fields nothing stronger set
maxSize = 2048
```

## Decision

### 1. The file

- One file per project, `kiln.toml`. `kiln-cook --project <file>` names it. Without the flag,
  `kiln-cook` uses `./kiln.toml` if it exists; it does not search parent directories.
- The provider takes `ProviderDesc::projectFile` (a path). The provider loads and owns the parsed
  project, so it can reload it (§6). A host without a file leaves it empty: nothing changes.
- Paths in the file are relative to the file's directory.
- Every section is optional. An empty file is valid and equals no file.

### 2. Syntax and parser

The sidecar parser (`src/cook/toml_subset.cpp`) grows into the project parser. The rule stays:
**every file kiln accepts is valid TOML**, and anything outside the subset is K3005 with file and
line. No new dependency.

Added to the subset:
- Arrays of scalars, on one line or several (`["a", "b"]`, a trailing comma allowed).
- Arrays of tables (`[[texture.rule]]`).
- Table headers of any depth (`[texture.preset.ui]`); quoted header keys (`[texture.preset."ui-2"]`).

Still not supported: inline tables, dotted keys, multi-line strings, dates and times. Sidecars
keep their current syntax: the new forms are K3005 in a sidecar.

*Why not a full TOML library (toml++ or similar):* the subset covers every form the file needs, the
parser exists (step 1 adds its fuzz target), and a library would add a cook-side dependency, a
header-heavy build, and exception handling to wrap. If users ask for the rest of TOML, the parser
can grow or the library can come in behind the same `Project` struct.

### 3. Sections

| Section | Contents |
|---|---|
| `[roots]` | `<name> = "<dir>"`, one key per root, as `--root [name=]dir`. The key `default` is the default root (names without `root:`); any other key is a root name (`[a-z0-9_]`, two or more characters). Read by `kiln-cook` only; any `--root` flag replaces the whole table |
| `[project]` | `store` (as `--store`), `target` (as `--target`). Read by `kiln-cook` only; its own flags win |

`[roots]` and `[project]` are ignored by the provider: the host's `ContextDesc` already names the
roots and the store. `default` becomes a reserved root name everywhere (`check_root_name`), so
the file and `--root` cannot disagree on what it means; a root named `default` was valid before,
so this is a CHANGELOG break.
| `[texture]`, `[mesh]` | project defaults: the sidecar keys of `settings.md` |
| `[texture.preset.<name>]`, `[mesh.preset.<name>]` | a named patch: the same keys. No preset inherits from another |
| `[[texture.rule]]`, `[[mesh.rule]]` | `match` (array of globs, required), optional `preset`, optional `targets` (array of profile names), and setting keys |
| `[texture.usage.<usage>]` | a patch for textures whose resolved usage is `<usage>` (§4) |
| `[target.<name>]` | reserved for project profiles (§5); K3006 until then |

Keys are the sidecar keys, so one key means the same thing in a sidecar, a preset, a rule and
the defaults. An unknown key, preset, usage or profile name, or a rule without `match`, is K3006.

**Globs** match the full asset name (`root:path/file.ext#sub`), byte for byte, as names are
compared. `*` matches within one path segment, `**` matches any number of segments, `?` matches one
character. A pattern without `root:` matches the default root only. `#` is an ordinary character,
so `"*.glb#*"` matches every embedded image of a glb in the folder. This gives embedded images the
per-asset settings that sidecars cannot.

**The first matching rule wins** (owner, 2026-10-01). Rules are tried in file order; the first
one whose globs match the asset applies, its preset first, then its own keys, and no later rule
is tried. So specific rules come first and a catch-all `**`, if any, comes last. A rule with
`targets` matches only when the cook's profile is in the list. An asset no rule matches gets the
project defaults (and usage sections) only.

### 4. Place in the resolution order

The decided order (`settings.md`, "Resolution layers") gains the project layers as 3a to 3d.
Each beats the layers above it:

| # | Layer | Kind |
|---|---|---|
| 1 | built-in defaults | data |
| 2 | host settings (`ProviderDesc` structs) | data, base |
| 3a | project defaults `[texture]` / `[mesh]` | patch |
| 3b | project usage sections `[texture.usage.*]`, evaluated late (below) | patch |
| 3c | the first matching project rule, with its preset | patch |
| 3d | `kiln-cook` setting flags (`--quality`, `--no-mips`, ...) | patch |
| 4 | sidecar | patch |
| 5 | inference (glTF slot, name rules) | code |
| 6 | `CookPolicy` | code |
| - | resolve | kiln |

- `kiln-cook` flags move from layer 2 to 3d. A flag is explicit, so it must beat the project
  file. A sidecar still beats a flag, as today. With no project file, the result is the same as now.
- **Usage sections (3b)** rank below rules but need the usage, which inference sets at layer 5.
  So they are evaluated after inference and patch only the fields that no layer from 3c to 5 set.
  The patches already know which fields they set, so this needs no new state. A usage section may
  not set `usage` (K3006).
- The policy still runs last and sees the project's result.

### 5. Targets

- Project profiles wait for the v0.9 mobile profiles (owner, 2026-10-01). Proposed shape then:
  `[target.<name>]` with `base` (a built-in profile) and the `TargetProfile` fields, selected by
  name like a built-in one. Until then the section name is reserved.
- Profile-specific settings use rules with `targets`, not a section per profile, so there is one
  way to say "this setting, for these assets".
- The usage → format table stays code. A project that wants another format for some assets sets
  `encoding` in a rule; a format outside the profile's `blockFormats` stays a K3002 error.

### 6. Staleness and hot reload

- The provider and `kiln-cook` fold a digest of the parsed project (its content, not the file's
  bytes, so comments do not count) into the host digest (`host_digest()` in `src/cook/unit.cpp`).
- That existing mechanism does the rest. When the digest differs from a record's, the record check
  resolves the unit's settings again and compares its outputs' build keys
  (`recorded_keys_match`). Only units whose resolved settings changed cook again. An edit to one
  rule re-cooks the assets it matches, not the store.
- The file is not an input in each record: its effect reaches every unit through the settings
  hash, and the check above costs one resolve per unit.
- **Hot reload:** the provider's source poller also stats the project file. On a change it parses
  the file again. If the new file has an error, it logs it and keeps the previous project. If not,
  it swaps the project in, and every unit checked this session is checked again under the new
  digest. A unit whose keys changed cooks again, and the manifest is rewritten once. `kiln-cook
  --watch` does the same.

### 7. `kiln-cook --explain <asset>`

Prints the resolved settings of one asset for the selected profile, one line per field, with the
layer that set it:

```
props/crate.png  (texture, profile desktop)
  usage        color     inferred: default (no slot, no name rule)
  maxSize      2048      rule #2 (kiln.toml:27)
  alphaCutoff  0.5       preset foliage (rule #2, kiln.toml:25)
  quality      high      project [texture] (kiln.toml:9)
  encoding     bc7       resolve: usage table for desktop
  genMips      true      default
```

The resolve functions get an optional out parameter that records the layer per field as the
patches apply. `--explain` cooks nothing and needs no store.

### 8. API

- `kiln/cook/project.h`: `Result<Project*> load_project(StrView path, Allocator const*,
  DiagSink const*)` and `free_project(Project*)`.
- `ResolveDesc` gains `Project const* project` (nullptr: no project layers).
- `ProviderDesc::projectFile` (§1). `cook_cli_main` takes `--project` like the other flags.
- The parsed `Project` is plain data (strings, arrays of patches, compiled globs), allocated with
  the caller's allocator, immutable after load, so worker threads read it without locks.

### 9. Diagnostics

| Code | Severity | Meaning |
|---|---|---|
| K3005 | Error | outside the TOML subset (existing; now also for the project file) |
| K3006 | Error | unknown key, preset or usage, a profile name in `targets` that is not built in; wrong type; a rule without `match` (existing code, new cases) |
| K3011 | Error | a `match` pattern that is not a valid glob |
| K3012 | Error | the project file cannot be read |
| - | Warning | `kiln-cook --check`: a rule that matches no source under the scanned roots (not built yet, R15) |

## Alternatives considered

| Alternative | Why not |
|---|---|
| A full TOML library | See §2: a dependency for forms the file does not need |
| A config per directory (`.kiln` files up the tree) | Spreads one project's rules over many files, and makes "which file set this" harder; rules with globs cover it |
| Every matching rule applies, later wins per key (composition) | Proposed first; rejected by the owner: an asset's settings then depend on every rule above it, which is hard to read and to review. First match keeps one rule per asset |
| Project file beats `kiln-cook` flags | A flag typed for one run would be silently ignored |
| Per-usage defaults as an ordinary early layer | Inference sets the usage later, so a usage patch cannot run before it |
| A section per target (`[target.desktop.texture]`) | A second way to scope settings next to rules |
| Usage → format table in the file | Owner decision: the table is code (`target-profiles.md`) |

## Rollout

1. Parser: arrays, arrays of tables, deeper and quoted headers; tests and a new fuzz target
   (`fuzz/fuzz_toml_subset.cpp`).
2. `Project` and `load_project`: defaults, presets, rules, globs; `ResolveDesc::project`; layers 3a,
   3c, 3d in `resolve_*_layers`; `kiln-cook --project`, `[roots]` and `[project]`; `default` reserved.
3. Host digest and the record check; the provider's `projectFile` and the poller (§6).
4. `--explain`.
5. Usage sections (3b) and `targets` on rules.
6. Docs: `settings.md` layer table, `cook-settings.md`, CHANGELOG (the flags' new layer).

Each step builds and passes on its own; steps 1 to 3 make the file useful.

## Owner decisions (2026-10-01)

1. `[roots]` (a table of `name = "dir"`, `default` for the default root) and `[project]` (store,
   target) configure `kiln-cook`; its own flags win.
2. `kiln-cook` setting flags move to layer 3d: above the project file, below sidecars.
3. The first matching rule wins (composition was proposed first and rejected).
4. A glob without `root:` matches the default root only.
5. Usage sections (`[texture.usage.<usage>]`) are kept: per-kind defaults that rules cannot
   express, because a glb's embedded images share a path.
6. Project profiles wait for the v0.9 mobile profiles.
