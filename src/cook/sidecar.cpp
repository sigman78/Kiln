// src/cook/sidecar.cpp — maps `.kiln` sidecar keys onto the cook settings structs.
// Keys are the struct field names; see docs/design/settings.md, "Sidecar files".
#include "kiln/cook/sidecar.h"

#include "settings_keys.h"
#include "toml_subset.h"

namespace kiln::cook {

namespace {

using detail::KeyError;
using detail::TomlEntry;

template <class Settings>
Status apply(StrView text, Settings* s, DiagSink const* diag, StrView file,
             SettingsTrace const* trace = nullptr) {
    Arena arena(Arena::Desc{default_allocator(), 4096, Tag::Cook});
    detail::TomlDoc doc(default_allocator());
    KILN_TRY(detail::parse_toml_subset(text, detail::TomlSyntax::Sidecar, arena, doc, diag, file));

    KeyError const err{diag, file};
    Settings next = *s;
    for (TomlEntry const& e : doc.entries) {
        if (!e.section.empty()) return err(e, "tables are not used in .kiln files yet");
        KILN_TRY(set_field(e, next, err));
        detail::trace_entry(trace, e, "sidecar", file);
    }
    *s = next;
    return kOk;
}

} // namespace

Status apply_sidecar(StrView text, TextureCookSettings* s, DiagSink const* diag, StrView file) {
    return apply(text, s, diag, file);
}

Status apply_sidecar(StrView text, MeshCookSettings* s, DiagSink const* diag, StrView file) {
    return apply(text, s, diag, file);
}

namespace detail {

Status apply_sidecar_traced(StrView text, TextureCookSettings* s, DiagSink const* diag, StrView file,
                            SettingsTrace const* trace) {
    return apply(text, s, diag, file, trace);
}

Status apply_sidecar_traced(StrView text, MeshCookSettings* s, DiagSink const* diag, StrView file,
                            SettingsTrace const* trace) {
    return apply(text, s, diag, file, trace);
}

} // namespace detail

} // namespace kiln::cook
