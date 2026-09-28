// kiln/cook/sidecar.h — per-asset `<source>.kiln` sidecar files (settings resolution layer 6).
// Syntax and keys: docs/design/settings.md, "Sidecar files".
#pragma once

#include "kiln/cook/settings.h"

namespace kiln::cook {

/// Sidecar file extension, appended to the full source file name: `wood_n.png.kiln`.
inline constexpr StrView kSidecarExt = ".kiln";

/// Sets the fields of `*s` that the sidecar `text` names; other fields keep their values.
/// `file` names the sidecar in diagnostics. On failure `*s` is unchanged: K3005 for text
/// outside the TOML subset, K3006 for an unknown key or a value of the wrong type or range.
KILN_API Status apply_sidecar(StrView text, TextureCookSettings* s, DiagSink const* diag = nullptr,
                              StrView file = {}) noexcept;
KILN_API Status apply_sidecar(StrView text, MeshCookSettings* s, DiagSink const* diag = nullptr,
                              StrView file = {}) noexcept;

} // namespace kiln::cook
