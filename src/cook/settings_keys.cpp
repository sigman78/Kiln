// src/cook/settings_keys.cpp — the settings keys of sidecars and kiln.toml: the struct field names.
// Keys and values: docs/design/settings.md, "Sidecar files".
#include "settings_keys.h"

#include "kiln/log.h"

#include <limits>

namespace kiln::cook::detail {

Status KeyError::operator()(TomlEntry const& e, char const* what) const {
    char where[1100];
    format(where, sizeof where, "%.*s:%u", KILN_SV(file), e.line);
    return diagf(diag, make_status(Code::InvalidArgument), kDiagSidecarKey, Severity::Error, file, where,
                 "'%.*s': %s", KILN_SV(e.key), what);
}

namespace {

template <class E> struct EnumName {
    StrView name;
    E value;
};

template <class E, usize N>
Status set_enum(TomlEntry const& e, EnumName<E> const (&names)[N], E& field, KeyError const& err) {
    if (e.type != TomlType::String) return err(e, "expected a string");
    for (EnumName<E> const& n : names)
        if (n.name == e.str) {
            field = n.value;
            return kOk;
        }
    return err(e, "unknown value");
}

Status set_bool(TomlEntry const& e, bool& field, KeyError const& err) {
    if (e.type != TomlType::Bool) return err(e, "expected true or false");
    field = e.b;
    return kOk;
}

Status set_u32(TomlEntry const& e, u32& field, KeyError const& err) {
    if (e.type != TomlType::Int) return err(e, "expected an integer");
    if (e.i < 0 || e.i > i64(std::numeric_limits<u32>::max())) return err(e, "out of range");
    field = u32(e.i);
    return kOk;
}

Status set_zstd_level(TomlEntry const& e, u8& field, KeyError const& err) {
    if (e.type != TomlType::Int) return err(e, "expected an integer");
    if (e.i < 0 || e.i > i64(kMaxZstdLevel)) return err(e, "out of range (0..19)");
    field = u8(e.i);
    return kOk;
}

/// An integer is accepted where a float is expected: `posTolMm = 1` reads as 1.0.
Status set_f32(TomlEntry const& e, f32& field, KeyError const& err) {
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
constexpr EnumName<TextureEncoding> kEncodings[] = {
    {"auto",         TextureEncoding::Auto        },
    {"uncompressed", TextureEncoding::Uncompressed},
    {"bc1",          TextureEncoding::BC1         },
    {"bc3",          TextureEncoding::BC3         },
    {"bc4",          TextureEncoding::BC4         },
    {"bc5",          TextureEncoding::BC5         },
    {"bc6h",         TextureEncoding::BC6H        },
    {"bc7",          TextureEncoding::BC7         },
};
constexpr EnumName<EncodeQuality> kQualities[] = {
    {"fast",   EncodeQuality::Fast  },
    {"normal", EncodeQuality::Normal},
    {"high",   EncodeQuality::High  },
};
constexpr EnumName<Supercompression> kSupercompressions[] = {
    {"none", Supercompression::None},
    {"zstd", Supercompression::Zstd},
};
constexpr EnumName<CompressionScheme> kCompressions[] = {
    {"none",         CompressionScheme::None       },
    {"meshopt",      CompressionScheme::Meshopt    },
    {"meshopt-zstd", CompressionScheme::MeshoptZstd},
};
constexpr EnumName<VertexProfile> kProfiles[] = {
    {"default", VertexProfile::Default},
    {"precise", VertexProfile::Precise},
    {"float",   VertexProfile::Float  },
};

} // namespace

Status set_field(TomlEntry const& e, TextureCookSettings& s, KeyError const& err) {
    if (e.key == "usage") return set_enum(e, kUsages, s.usage, err);
    if (e.key == "colorSpace") return set_enum(e, kColorSpaces, s.colorSpace, err);
    if (e.key == "genMips") return set_bool(e, s.genMips, err);
    if (e.key == "normalRenormalize") return set_bool(e, s.normalRenormalize, err);
    if (e.key == "maxSize") return set_u32(e, s.maxSize, err);
    if (e.key == "flipGreen") return set_bool(e, s.flipGreen, err);
    if (e.key == "shape") return set_enum(e, kShapes, s.shape, err);
    if (e.key == "encoding") return set_enum(e, kEncodings, s.encoding, err);
    if (e.key == "quality") return set_enum(e, kQualities, s.quality, err);
    if (e.key == "slices") return set_u32(e, s.slices, err);
    if (e.key == "supercompression") return set_enum(e, kSupercompressions, s.supercompression, err);
    if (e.key == "zstdLevel") return set_zstd_level(e, s.zstdLevel, err);
    if (e.key == "alphaCutoff") return set_f32(e, s.alphaCutoff, err);
    return err(e, "unknown key for a texture");
}

Status set_field(TomlEntry const& e, MeshCookSettings& s, KeyError const& err) {
    if (e.key == "profile") return set_enum(e, kProfiles, s.profile, err);
    if (e.key == "genTangents") return set_bool(e, s.genTangents, err);
    if (e.key == "optimize") return set_bool(e, s.optimize, err);
    if (e.key == "useAuthoredLods") return set_bool(e, s.useAuthoredLods, err);
    if (e.key == "posTolMm") return set_f32(e, s.posTolMm, err);
    if (e.key == "weldTol") return set_f32(e, s.weldTol, err);
    if (e.key == "compression") return set_enum(e, kCompressions, s.compression, err);
    if (e.key == "zstdLevel") return set_zstd_level(e, s.zstdLevel, err);
    return err(e, "unknown key for a mesh");
}

namespace {

template <class E, usize N> StrView enum_name(EnumName<E> const (&names)[N], E v) {
    for (EnumName<E> const& n : names)
        if (n.value == v) return n.name;
    return "?";
}

} // namespace

constexpr StrView kTextureKeys[] = {
    "usage",  "colorSpace", "genMips", "normalRenormalize", "maxSize",   "flipGreen",  "shape",
    "slices", "encoding",   "quality", "supercompression",  "zstdLevel", "alphaCutoff"};
constexpr StrView kMeshKeys[] = {"profile",  "genTangents", "optimize",    "useAuthoredLods",
                                 "posTolMm", "weldTol",     "compression", "zstdLevel"};

bool usage_from_text(StrView text, TextureUsage* out) {
    for (EnumName<TextureUsage> const& n : kUsages)
        if (n.name == text) {
            *out = n.value;
            return true;
        }
    return false;
}

u32 key_bit(AssetKind kind, StrView key) {
    Span<StrView const> const keys = setting_keys(kind);
    for (usize i = 0; i < keys.size; ++i)
        if (keys[i] == key) return 1u << i;
    return 0;
}

Span<StrView const> setting_keys(AssetKind kind) {
    return kind == AssetKind::Mesh ? Span<StrView const>(kMeshKeys) : Span<StrView const>(kTextureKeys);
}

usize field_text(TextureCookSettings const& s, StrView key, char* out, usize cap) {
    auto const text = [&](StrView v) { return format(out, cap, "%.*s", KILN_SV(v)); };
    auto const flag = [&](bool v) { return text(v ? "true" : "false"); };
    if (key == "usage") return text(enum_name(kUsages, s.usage));
    if (key == "colorSpace") return text(enum_name(kColorSpaces, s.colorSpace));
    if (key == "genMips") return flag(s.genMips);
    if (key == "normalRenormalize") return flag(s.normalRenormalize);
    if (key == "maxSize") return format(out, cap, "%u", s.maxSize);
    if (key == "flipGreen") return flag(s.flipGreen);
    if (key == "shape") return text(enum_name(kShapes, s.shape));
    if (key == "slices") return format(out, cap, "%u", s.slices);
    if (key == "encoding") return text(enum_name(kEncodings, s.encoding));
    if (key == "quality") return text(enum_name(kQualities, s.quality));
    if (key == "supercompression") return text(enum_name(kSupercompressions, s.supercompression));
    if (key == "zstdLevel") return format(out, cap, "%u", unsigned(s.zstdLevel));
    if (key == "alphaCutoff")
        return s.alphaCutoff < 0.0f ? text("auto") : format(out, cap, "%g", double(s.alphaCutoff));
    return text("?");
}

usize field_text(MeshCookSettings const& s, StrView key, char* out, usize cap) {
    auto const text = [&](StrView v) { return format(out, cap, "%.*s", KILN_SV(v)); };
    auto const flag = [&](bool v) { return text(v ? "true" : "false"); };
    if (key == "profile") return text(enum_name(kProfiles, s.profile));
    if (key == "genTangents") return flag(s.genTangents);
    if (key == "optimize") return flag(s.optimize);
    if (key == "useAuthoredLods") return flag(s.useAuthoredLods);
    if (key == "posTolMm") return format(out, cap, "%g", double(s.posTolMm));
    if (key == "weldTol") return format(out, cap, "%g", double(s.weldTol));
    if (key == "compression") return text(enum_name(kCompressions, s.compression));
    if (key == "zstdLevel") return format(out, cap, "%u", unsigned(s.zstdLevel));
    return text("?");
}

void trace_changes(TextureCookSettings const& before, TextureCookSettings const& after,
                   SettingsTrace const* trace, StrView layer, StrView where) {
    if (!trace || !trace->fn) return;
    for (StrView const key : kTextureKeys) {
        char a[64], b[64];
        if (StrView(a, field_text(before, key, a, sizeof a)) !=
            StrView(b, field_text(after, key, b, sizeof b)))
            trace->fn(trace->user, key, layer, where);
    }
}

void trace_changes(MeshCookSettings const& before, MeshCookSettings const& after, SettingsTrace const* trace,
                   StrView layer, StrView where) {
    if (!trace || !trace->fn) return;
    for (StrView const key : kMeshKeys) {
        char a[64], b[64];
        if (StrView(a, field_text(before, key, a, sizeof a)) !=
            StrView(b, field_text(after, key, b, sizeof b)))
            trace->fn(trace->user, key, layer, where);
    }
}

void trace_entry(SettingsTrace const* trace, TomlEntry const& e, StrView layer, StrView file) {
    if (!trace || !trace->fn) return;
    char where[1100];
    usize const n = format(where, sizeof where, "%.*s:%u", KILN_SV(file), e.line);
    trace->fn(trace->user, e.key, layer, StrView(where, n < sizeof where ? n : sizeof where - 1));
}

} // namespace kiln::cook::detail
