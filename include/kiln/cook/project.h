// kiln/cook/project.h — the project file `kiln.toml`: defaults, presets and path rules (settings
// layers 3a to 3d). Format and semantics: docs/design/project-config.md.
#pragma once

#include "kiln/assets.h"
#include "kiln/cook/settings.h"

namespace kiln::cook {

/// The project file name `kiln-cook` looks for in the working directory.
inline constexpr StrView kProjectFileName = "kiln.toml";

/// A parsed project. Immutable after load_project(), so worker threads read it without locks.
struct Project;

struct ProjectDesc {
    StrView path = {}; ///< the project file; empty: none (only `overrides`)
    /// Layer 3d: TOML with `[texture]` and `[mesh]` tables, the settings kiln-cook's flags give.
    /// They beat the project file and yield to sidecars.
    StrView overrides = {};
};

/// Reads and checks the project file. K3005 for text outside kiln's TOML subset; K3006 for an
/// unknown table, key or value, a preset that does not exist, or a rule without `match`; K3011 for
/// an invalid glob. IoError or NotFound if the file cannot be read.
KILN_API Result<Project*> load_project(ProjectDesc const& desc, Allocator const* alloc = nullptr,
                                       DiagSink const* diag = nullptr);
/// Null is a no-op.
KILN_API void free_project(Project* p);

/// `[roots]`: one root per key, `default` for the default root; dirs relative to the file's
/// directory are made relative to the working directory, without a trailing separator. Empty
/// without a file.
KILN_API Span<Root const> project_roots(Project const* p);
/// `[project] store` and `target` (store resolved like a root dir), or empty.
KILN_API StrView project_store(Project const* p);
KILN_API StrView project_target(Project const* p);

/// A digest of everything that can change resolved settings (not roots, store or comments). The
/// cook folds it into the host digest, so a change makes recorded keys checked again.
KILN_API u64 project_digest(Project const* p);

/// True if `pattern` (a `match` glob) matches the asset name `name`. `*` matches within a path
/// segment, `**` any number of segments, `?` one character but `/`. A pattern without `root:`
/// matches names of the default root only. For tests and tools; rules call it internally.
[[nodiscard]] KILN_API bool glob_match(StrView pattern, StrView name);

} // namespace kiln::cook
