// src/cook/toml_subset.h — parser for kiln's strict TOML subset: `.kiln` sidecars and `kiln.toml`.
// Syntax: docs/design/settings.md, "Sidecar files", and docs/design/project-config.md §2.
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/result.h"

namespace kiln::cook::detail {

enum class TomlType : u8 { String, Int, Float, Bool, Array };

/// Sidecar: scalars and `[a.b]` headers only. Project adds arrays of scalars, `[[a.b]]` and quoted
/// header segments.
enum class TomlSyntax : u8 { Sidecar, Project };

struct TomlValue {
    TomlType type = TomlType::Bool;
    StrView str; ///< String: the decoded value
    i64 i  = 0;  ///< Int
    f64 f  = 0.0;
    bool b = false;
};

struct TomlEntry : TomlValue {
    u32 table = 0;   ///< index into TomlDoc::tables; 0 is the top level
    StrView section; ///< the table's name, e.g. "a.b"; "" at top level
    StrView key;
    Span<TomlValue const> items; ///< Array: scalars of one type
    u32 line = 0;                ///< 1-based
};

/// One header in file order; each `[[a.b]]` is a table of its own. Table 0 is the top level.
struct TomlTable {
    StrView name;
    bool array = false; ///< from `[[name]]`
    u32 line   = 0;
};

struct TomlDoc {
    Vec<TomlTable> tables;
    Vec<TomlEntry> entries; ///< file order

    explicit TomlDoc(Allocator const* a) : tables(a, Tag::Cook), entries(a, Tag::Cook) {}
};

/// Parses `text` into `out`. Decoded strings, names and arrays live in `arena`. Anything outside the
/// subset, or invalid TOML, is a K3005 error whose `where` is "<file>:<line>".
Status parse_toml_subset(StrView text, TomlSyntax syntax, Arena& arena, TomlDoc& out, DiagSink const* diag,
                         StrView file);

} // namespace kiln::cook::detail
