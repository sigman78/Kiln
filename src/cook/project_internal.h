// src/cook/project_internal.h — how the resolve step applies a project (layers 3a to 3d).
#pragma once

#include "kiln/cook/project.h"

namespace kiln::cook::detail {

/// Project defaults, then the first matching rule (its preset, then its keys), then the overrides.
/// The project was checked at load, so this fails only on a bug.
Status apply_project(Project const& p, CookAssetInfo const& asset, TargetProfile const& target,
                     TextureCookSettings* s, DiagSink const* diag);
Status apply_project(Project const& p, CookAssetInfo const& asset, TargetProfile const& target,
                     MeshCookSettings* s, DiagSink const* diag);

} // namespace kiln::cook::detail
