// src/cook/project_internal.h — how the resolve step applies a project (layers 3a to 3d).
#pragma once

#include "kiln/cook/project.h"

namespace kiln::cook::detail {

/// Layer 3a, the project defaults. The project was checked at load, so these fail only on a bug.
Status apply_project_defaults(Project const& p, TextureCookSettings* s, DiagSink const* diag,
                              SettingsTrace const* trace);
Status apply_project_defaults(Project const& p, MeshCookSettings* s, DiagSink const* diag,
                              SettingsTrace const* trace);
/// Layers 3c and 3d: the first matching rule (its preset, then its keys), then the overrides.
Status apply_project_rules(Project const& p, CookAssetInfo const& asset, TargetProfile const& target,
                           TextureCookSettings* s, DiagSink const* diag, SettingsTrace const* trace);
Status apply_project_rules(Project const& p, CookAssetInfo const& asset, TargetProfile const& target,
                           MeshCookSettings* s, DiagSink const* diag, SettingsTrace const* trace);
/// Layer 3b, evaluated after inference: the `[texture.usage.<s.usage>]` keys whose bit is not in
/// `taken` (the keys layers 3c to 5 set).
Status apply_project_usage(Project const& p, TextureCookSettings* s, u32 taken, DiagSink const* diag,
                           SettingsTrace const* trace);

} // namespace kiln::cook::detail
