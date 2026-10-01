// src/cook/settings_keys.h — the settings keys shared by `.kiln` sidecars and kiln.toml.
#pragma once

#include "toml_subset.h"

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

} // namespace kiln::cook::detail
