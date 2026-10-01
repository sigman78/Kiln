// src/cook/settings_keys.h — the settings keys shared by `.kiln` sidecars and kiln.toml.
#pragma once

#include "toml_subset.h"

#include "kiln/assets.h"
#include "kiln/cook/settings.h"

namespace kiln::cook::detail {

/// Reports a bad key or value as K3006 at "<file>:<line>".
struct KeyError {
    DiagSink const* diag;
    StrView file;

    Status operator()(TomlEntry const& e, char const* what) const;
};

/// Sets the field that `e.key` names. K3006 for an unknown key or a value of the wrong type or range.
Status set_field(TomlEntry const& e, TextureCookSettings& s, KeyError const& err);
Status set_field(TomlEntry const& e, MeshCookSettings& s, KeyError const& err);

/// The keys of one kind, in the order kiln-cook --explain prints them.
Span<StrView const> setting_keys(AssetKind kind);
/// The value of `key` as a sidecar would write it (enum names unquoted).
usize field_text(TextureCookSettings const& s, StrView key, char* out, usize cap);
usize field_text(MeshCookSettings const& s, StrView key, char* out, usize cap);

/// Reports to `trace` every key whose value differs between `before` and `after`.
void trace_changes(TextureCookSettings const& before, TextureCookSettings const& after,
                   SettingsTrace const* trace, StrView layer, StrView where);
void trace_changes(MeshCookSettings const& before, MeshCookSettings const& after, SettingsTrace const* trace,
                   StrView layer, StrView where);
/// Reports that `e` set its key, at "<file>:<line>".
void trace_entry(SettingsTrace const* trace, TomlEntry const& e, StrView layer, StrView file);

/// apply_sidecar() that reports each key it sets to `trace` (may be null).
Status apply_sidecar_traced(StrView text, TextureCookSettings* s, DiagSink const* diag, StrView file,
                            SettingsTrace const* trace);
Status apply_sidecar_traced(StrView text, MeshCookSettings* s, DiagSink const* diag, StrView file,
                            SettingsTrace const* trace);

} // namespace kiln::cook::detail
