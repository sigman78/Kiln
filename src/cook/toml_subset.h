// src/cook/toml_subset.h — parser for the strict TOML subset of `.kiln` sidecar files.
// Syntax: docs/design/settings.md, "Sidecar files".
#pragma once

#include "kiln/alloc.h"
#include "kiln/containers.h"
#include "kiln/result.h"

namespace kiln::cook::detail {

enum class TomlType : u8 { String, Int, Float, Bool };

struct TomlEntry {
    StrView section; ///< "" at top level, else the header without brackets, e.g. "a.b"
    StrView key;
    TomlType type = TomlType::Bool;
    StrView str;    ///< String: the decoded value
    i64 i    = 0;   ///< Int
    f64 f    = 0.0; ///< Float
    bool b   = false;
    u32 line = 0; ///< 1-based
};

/// Parses `text` into `out` in file order. Decoded strings and sections live in `arena`.
/// Anything outside the subset is a K3005 error whose `where` is "<file>:<line>".
Status parse_toml_subset(StrView text, Arena& arena, Vec<TomlEntry>& out, DiagSink const* diag, StrView file);

} // namespace kiln::cook::detail
