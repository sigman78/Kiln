// src/cook/sidecar.cpp — maps `.kiln` sidecar keys onto the cook settings structs.
// Keys are the struct field names; see docs/design/settings.md, "Sidecar files".
#include "kiln/cook/sidecar.h"

#include "toml_subset.h"

#include "kiln/log.h"

#include <limits>

namespace kiln::cook {

namespace {

using detail::TomlEntry;
using detail::TomlType;

struct KeyError {
    DiagSink const* diag;
    StrView file;

    Status operator()(TomlEntry const& e, char const* what) const noexcept {
        char where[1100];
        format(where, sizeof where, "%.*s:%u", KILN_SV(file), e.line);
        return diagf(diag, make_status(Code::InvalidArgument), kDiagSidecarKey, Severity::Error, file, where,
                     "'%.*s': %s", KILN_SV(e.key), what);
    }
};

template <class E> struct EnumName {
    StrView name;
    E value;
};

template <class E, usize N>
Status set_enum(TomlEntry const& e, EnumName<E> const (&names)[N], E& field, KeyError const& err) noexcept {
    if (e.type != TomlType::String) return err(e, "expected a string");
    for (EnumName<E> const& n : names)
        if (n.name == e.str) {
            field = n.value;
            return kOk;
        }
    return err(e, "unknown value");
}

Status set_bool(TomlEntry const& e, bool& field, KeyError const& err) noexcept {
    if (e.type != TomlType::Bool) return err(e, "expected true or false");
    field = e.b;
    return kOk;
}

Status set_u32(TomlEntry const& e, u32& field, KeyError const& err) noexcept {
    if (e.type != TomlType::Int) return err(e, "expected an integer");
    if (e.i < 0 || e.i > i64(std::numeric_limits<u32>::max())) return err(e, "out of range");
    field = u32(e.i);
    return kOk;
}

/// An integer is accepted where a float is expected: `posTolMm = 1` reads as 1.0.
Status set_f32(TomlEntry const& e, f32& field, KeyError const& err) noexcept {
    if (e.type == TomlType::Int)
        field = f32(e.i);
    else if (e.type == TomlType::Float)
        field = f32(e.f);
    else
        return err(e, "expected a number");
    return kOk;
}

constexpr EnumName<TextureUsage> kUsages[] = {
    {"auto",   TextureUsage::Auto  },
    {"color",  TextureUsage::Color },
    {"normal", TextureUsage::Normal},
    {"orm",    TextureUsage::Orm   },
    {"mask",   TextureUsage::Mask  },
    {"hdr",    TextureUsage::Hdr   },
    {"ui",     TextureUsage::Ui    },
    {"lut",    TextureUsage::Lut   },
    {"height", TextureUsage::Height},
};
constexpr EnumName<ColorSpace> kColorSpaces[] = {
    {"auto",   ColorSpace::Auto  },
    {"srgb",   ColorSpace::Srgb  },
    {"linear", ColorSpace::Linear},
};
constexpr EnumName<CookShape> kShapes[] = {
    {"auto",  CookShape::Auto },
    {"2d",    CookShape::Tex2D},
    {"cube",  CookShape::Cube },
    {"array", CookShape::Array},
};
constexpr EnumName<VertexProfile> kProfiles[] = {
    {"default", VertexProfile::Default},
    {"precise", VertexProfile::Precise},
    {"float",   VertexProfile::Float  },
};

Status set_field(TomlEntry const& e, TextureCookSettings& s, KeyError const& err) noexcept {
    if (e.key == "usage") return set_enum(e, kUsages, s.usage, err);
    if (e.key == "colorSpace") return set_enum(e, kColorSpaces, s.colorSpace, err);
    if (e.key == "genMips") return set_bool(e, s.genMips, err);
    if (e.key == "normalRenormalize") return set_bool(e, s.normalRenormalize, err);
    if (e.key == "maxSize") return set_u32(e, s.maxSize, err);
    if (e.key == "flipGreen") return set_bool(e, s.flipGreen, err);
    if (e.key == "shape") return set_enum(e, kShapes, s.shape, err);
    if (e.key == "slices") return set_u32(e, s.slices, err);
    return err(e, "unknown key for a texture");
}

Status set_field(TomlEntry const& e, MeshCookSettings& s, KeyError const& err) noexcept {
    if (e.key == "profile") return set_enum(e, kProfiles, s.profile, err);
    if (e.key == "genTangents") return set_bool(e, s.genTangents, err);
    if (e.key == "optimize") return set_bool(e, s.optimize, err);
    if (e.key == "useAuthoredLods") return set_bool(e, s.useAuthoredLods, err);
    if (e.key == "posTolMm") return set_f32(e, s.posTolMm, err);
    if (e.key == "weldTol") return set_f32(e, s.weldTol, err);
    return err(e, "unknown key for a mesh");
}

template <class Settings>
Status apply(StrView text, Settings* s, DiagSink const* diag, StrView file) noexcept {
    Arena arena(Arena::Desc{default_allocator(), 4096, Tag::Cook});
    Vec<TomlEntry> entries(default_allocator(), Tag::Cook);
    KILN_TRY(detail::parse_toml_subset(text, arena, entries, diag, file));

    KeyError const err{diag, file};
    Settings next = *s;
    for (TomlEntry const& e : entries) {
        if (!e.section.empty()) return err(e, "tables are not used in .kiln files yet");
        KILN_TRY(set_field(e, next, err));
    }
    *s = next;
    return kOk;
}

} // namespace

Status apply_sidecar(StrView text, TextureCookSettings* s, DiagSink const* diag, StrView file) noexcept {
    return apply(text, s, diag, file);
}

Status apply_sidecar(StrView text, MeshCookSettings* s, DiagSink const* diag, StrView file) noexcept {
    return apply(text, s, diag, file);
}

} // namespace kiln::cook
