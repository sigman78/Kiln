// src/cook/project.cpp — kiln.toml: parse, check, and apply as settings layers 3a to 3d
// (docs/design/project-config.md).
#include "project_internal.h"

#include "settings_keys.h"
#include "toml_subset.h"

#include "kiln/hash.h"
#include "kiln/io.h"
#include "kiln/log.h"
#include "kiln/manifest.h"

#include <cstring>

namespace kiln::cook {

namespace detail {
namespace {

struct Range {
    u32 begin = 0, count = 0;
};

struct Preset {
    StrView name;
    Range keys;
};

struct Rule {
    Range match;   ///< into Project::strs
    Range targets; ///< into Project::strs; empty: every profile
    Range keys;
    i32 preset = -1;
    u32 line   = 0;
};

struct Kind {
    Range defaults;
    Range overrides;
    Range usage[u32(TextureUsage::Height) + 1]; ///< textures: [texture.usage.<name>]
    Vec<Preset> presets;
    Vec<Rule> rules;

    explicit Kind(Allocator const* a) : presets(a, Tag::Cook), rules(a, Tag::Cook) {}
};

} // namespace
} // namespace detail

struct Project {
    Allocator const* alloc;
    Arena arena;
    Vec<u8> text;         ///< the file: bare keys point into it
    Vec<char> overText;   ///< a copy of ProjectDesc::overrides, for the same reason
    detail::TomlDoc doc;  ///< the file
    detail::TomlDoc over; ///< the overrides
    Vec<detail::TomlEntry const*> keys;
    Vec<StrView> strs;
    detail::Kind tex, mesh;
    Vec<Root> roots;
    StrView store, target, file;
    u64 digest = 0;

    explicit Project(Allocator const* a)
        : alloc(a), arena(Arena::Desc{a, 4096, Tag::Cook}), text(a, Tag::Cook), overText(a, Tag::Cook),
          doc(a), over(a), keys(a, Tag::Cook), strs(a, Tag::Cook), tex(a), mesh(a), roots(a, Tag::Cook) {}
};

namespace detail {
namespace {

constexpr StrView kOverridesName = "kiln-cook flags";

bool is_absolute(StrView p) {
    return (!p.empty() && (p[0] == '/' || p[0] == '\\')) || (p.size >= 2 && p[1] == ':');
}

/// `rel` against the project file's directory, unless it is absolute. A trailing separator is
/// dropped, so "src/" and "src" name the same root.
StrView project_path(Project& p, StrView rel) {
    while (rel.size > 1 && (rel[rel.size - 1] == '/' || rel[rel.size - 1] == '\\') &&
           rel[rel.size - 2] != ':')
        rel = rel.substr(0, rel.size - 1);
    usize const fwd = p.file.rfind('/'), back = p.file.rfind('\\');
    usize const slash = fwd == StrView::kNpos ? back : back == StrView::kNpos || fwd > back ? fwd : back;
    if (is_absolute(rel) || slash == StrView::kNpos) return p.arena.copy(rel);
    char buf[1100];
    usize const n = format(buf, sizeof buf, "%.*s/%.*s", int(slash), p.file.data, KILN_SV(rel));
    return p.arena.copy(StrView(buf, n < sizeof buf ? n : sizeof buf - 1));
}

Status table_error(Project const& p, TomlTable const& t, char const* what, DiagSink const* diag) {
    char where[1100];
    format(where, sizeof where, "%.*s:%u", KILN_SV(p.file), t.line);
    return diagf(diag, make_status(Code::InvalidArgument), kDiagSidecarKey, Severity::Error, p.file, where,
                 "[%.*s]: %s", KILN_SV(t.name), what);
}

/// Null if `pattern` is a glob rules accept, else a reason.
char const* check_glob(StrView pattern) {
    if (pattern.empty()) return "empty pattern";
    usize const colon = pattern.find(':');
    StrView path      = pattern;
    if (colon != StrView::kNpos) {
        if (char const* why = check_root_name(pattern.substr(0, colon))) return why;
        path = pattern.substr(colon + 1);
        if (path.find(':') != StrView::kNpos) return "':' only after a root name";
    }
    if (path.empty()) return "empty path after the root";
    for (usize i = 0; i + 1 < path.size; ++i) {
        if (path[i] != '*' || path[i + 1] != '*') continue;
        bool const segStart = i == 0 || path[i - 1] == '/';
        bool const segEnd   = i + 2 == path.size || path[i + 2] == '/';
        if (!segStart || !segEnd) return "'**' must be a whole path segment";
        ++i;
    }
    if (path.find('\\') != StrView::kNpos) return "use '/' in patterns";
    return nullptr;
}

bool glob_path(StrView p, StrView s) {
    usize pi = 0, si = 0;
    while (pi < p.size) {
        if (pi + 1 < p.size && p[pi] == '*' && p[pi + 1] == '*') {
            pi += 2;
            if (pi < p.size && p[pi] == '/' && glob_path(p.substr(pi + 1), s.substr(si))) return true;
            for (usize k = si; k <= s.size; ++k)
                if (glob_path(p.substr(pi), s.substr(k))) return true;
            return false;
        }
        if (p[pi] == '*') {
            ++pi;
            for (usize k = si;; ++k) {
                if (glob_path(p.substr(pi), s.substr(k))) return true;
                if (k == s.size || s[k] == '/') return false;
            }
        }
        if (si >= s.size) return false;
        if (p[pi] == '?' ? s[si] == '/' : p[pi] != s[si]) return false;
        ++pi;
        ++si;
    }
    return si == s.size;
}

Range push_strings(Project& p, TomlEntry const& e) {
    Range r{u32(p.strs.size()), u32(e.items.size)};
    for (TomlValue const& v : e.items)
        p.strs.push_back(v.str);
    return r;
}

/// Collects the setting keys of table `t` into p.keys, checked against `Settings`. In a rule,
/// `match`, `preset` and `targets` are not settings and are skipped.
template <class Settings>
Status collect_keys(Project& p, TomlDoc const& doc, u32 t, Range* out, DiagSink const* diag, StrView file) {
    out->begin = u32(p.keys.size());
    for (TomlEntry const& e : doc.entries) {
        if (e.table != t) continue;
        if (doc.tables[t].array && (e.key == "match" || e.key == "preset" || e.key == "targets")) continue;
        Settings scratch;
        KILN_TRY(set_field(e, scratch, KeyError{diag, file}));
        p.keys.push_back(&e);
    }
    out->count = u32(p.keys.size()) - out->begin;
    return kOk;
}

i32 find_preset(Kind const& k, StrView name) {
    for (usize i = 0; i < k.presets.size(); ++i)
        if (k.presets[i].name == name) return i32(i);
    return -1;
}

template <class Settings> Status parse_rule(Project& p, Kind& k, u32 t, DiagSink const* diag) {
    TomlTable const& table = p.doc.tables[t];
    Rule r;
    r.line = table.line;
    KeyError const err{diag, p.file};
    bool hasMatch = false;
    for (TomlEntry const& e : p.doc.entries) {
        if (e.table != t) continue;
        if (e.key == "match") {
            if (e.type != TomlType::Array || e.items.size == 0 || e.items[0].type != TomlType::String)
                return err(e, "expected a non-empty array of strings");
            for (TomlValue const& v : e.items)
                if (char const* why = check_glob(v.str)) {
                    char where[1100];
                    format(where, sizeof where, "%.*s:%u", KILN_SV(p.file), e.line);
                    return diagf(diag, make_status(Code::InvalidArgument), kDiagProjectGlob, Severity::Error,
                                 p.file, where, "'%.*s': %s", KILN_SV(v.str), why);
                }
            r.match  = push_strings(p, e);
            hasMatch = true;
        } else if (e.key == "targets") {
            if (e.type != TomlType::Array || (e.items.size && e.items[0].type != TomlType::String))
                return err(e, "expected an array of profile names");
            for (TomlValue const& v : e.items)
                if (!target_profile(v.str))
                    return err(e, "not a built-in profile (compat, desktop, uncompressed)");
            r.targets = push_strings(p, e);
        } else if (e.key == "preset") {
            if (e.type != TomlType::String) return err(e, "expected a preset name");
            r.preset = find_preset(k, e.str);
            if (r.preset < 0) return err(e, "no such preset");
        }
    }
    if (!hasMatch) return table_error(p, table, "a rule needs 'match'", diag);
    KILN_TRY(collect_keys<Settings>(p, p.doc, t, &r.keys, diag, p.file));
    k.rules.push_back(r);
    return kOk;
}

/// The part of `name` after `prefix.`, or empty.
StrView child_of(StrView name, StrView prefix) {
    if (name.size <= prefix.size + 1 || !name.starts_with(prefix) || name[prefix.size] != '.') return {};
    return name.substr(prefix.size + 1);
}

/// The name of a preset table `<prefix>.<name>`, or empty: a deeper table is not a preset.
StrView preset_name(StrView table, StrView prefix) {
    StrView const n = child_of(table, prefix);
    return n.find('.') == StrView::kNpos ? n : StrView();
}

Status parse_roots(Project& p, u32 t, DiagSink const* diag) {
    KeyError const err{diag, p.file};
    for (TomlEntry const& e : p.doc.entries) {
        if (e.table != t) continue;
        if (e.type != TomlType::String || e.str.empty()) return err(e, "expected a directory");
        StrView name;
        if (e.key != "default") {
            if (char const* why = check_root_name(e.key)) return err(e, why);
            name = e.key;
        }
        p.roots.push_back(Root{name, project_path(p, e.str)});
    }
    return kOk;
}

Status parse_project_table(Project& p, u32 t, DiagSink const* diag) {
    KeyError const err{diag, p.file};
    for (TomlEntry const& e : p.doc.entries) {
        if (e.table != t) continue;
        if (e.type != TomlType::String || e.str.empty()) return err(e, "expected a string");
        if (e.key == "store")
            p.store = project_path(p, e.str);
        else if (e.key == "target")
            p.target = e.str;
        else
            return err(e, "unknown key (store, target)");
    }
    return kOk;
}

Status parse_file(Project& p, DiagSink const* diag) {
    KeyError const err{diag, p.file};
    for (TomlEntry const& e : p.doc.entries)
        if (e.table == 0) return err(e, "keys belong in a table such as [texture] or [mesh]");
    // Presets first: rules name them wherever they are in the file.
    for (u32 t = 1; t < p.doc.tables.size(); ++t) {
        StrView const name = p.doc.tables[t].name;
        if (StrView const n = preset_name(name, "texture.preset"); !n.empty()) {
            Preset pr{n, {}};
            KILN_TRY(collect_keys<TextureCookSettings>(p, p.doc, t, &pr.keys, diag, p.file));
            p.tex.presets.push_back(pr);
        } else if (StrView const m = preset_name(name, "mesh.preset"); !m.empty()) {
            Preset pr{m, {}};
            KILN_TRY(collect_keys<MeshCookSettings>(p, p.doc, t, &pr.keys, diag, p.file));
            p.mesh.presets.push_back(pr);
        }
    }
    for (u32 t = 1; t < p.doc.tables.size(); ++t) {
        TomlTable const& table = p.doc.tables[t];
        StrView const name     = table.name;
        if (!preset_name(name, "texture.preset").empty() || !preset_name(name, "mesh.preset").empty())
            continue;
        if (name == "roots") {
            KILN_TRY(parse_roots(p, t, diag));
        } else if (name == "project") {
            KILN_TRY(parse_project_table(p, t, diag));
        } else if (name == "texture") {
            KILN_TRY(collect_keys<TextureCookSettings>(p, p.doc, t, &p.tex.defaults, diag, p.file));
        } else if (name == "mesh") {
            KILN_TRY(collect_keys<MeshCookSettings>(p, p.doc, t, &p.mesh.defaults, diag, p.file));
        } else if (name == "texture.rule" && table.array) {
            KILN_TRY(parse_rule<TextureCookSettings>(p, p.tex, t, diag));
        } else if (name == "mesh.rule" && table.array) {
            KILN_TRY(parse_rule<MeshCookSettings>(p, p.mesh, t, diag));
        } else if (StrView const u = child_of(name, "texture.usage"); !u.empty()) {
            TextureUsage usage = TextureUsage::Auto;
            if (!usage_from_text(u, &usage) || usage == TextureUsage::Auto)
                return table_error(p, table, "not a usage (color, normal, orm, mask, hdr, ui, lut, height)",
                                   diag);
            for (TomlEntry const& e : p.doc.entries)
                if (e.table == t && e.key == "usage") return err(e, "a usage section cannot set the usage");
            KILN_TRY(collect_keys<TextureCookSettings>(p, p.doc, t, &p.tex.usage[u32(usage)], diag, p.file));
        } else if (name == "texture.preset" || name == "mesh.preset" || name == "texture.usage") {
            for (TomlEntry const& e : p.doc.entries)
                if (e.table == t) return err(e, "a preset is a table: [texture.preset.<name>]");
        } else if (name == "target" || !child_of(name, "target").empty()) {
            return table_error(p, table, "project profiles are reserved (docs/design/project-config.md)",
                               diag);
        } else {
            return table_error(p, table, "unknown table", diag);
        }
    }
    return kOk;
}

Status parse_overrides(Project& p, DiagSink const* diag) {
    for (TomlEntry const& e : p.over.entries)
        if (e.table == 0) return KeyError{diag, kOverridesName}(e, "overrides belong in [texture] or [mesh]");
    for (u32 t = 1; t < p.over.tables.size(); ++t) {
        StrView const name = p.over.tables[t].name;
        if (name == "texture")
            KILN_TRY(collect_keys<TextureCookSettings>(p, p.over, t, &p.tex.overrides, diag, kOverridesName));
        else if (name == "mesh")
            KILN_TRY(collect_keys<MeshCookSettings>(p, p.over, t, &p.mesh.overrides, diag, kOverridesName));
        else
            return diagf(diag, make_status(Code::InvalidArgument), kDiagSidecarKey, Severity::Error,
                         kOverridesName, kOverridesName, "[%.*s]: overrides have [texture] and [mesh] only",
                         KILN_SV(name));
    }
    return kOk;
}

void hash_range(Xxh64State& h, Project const& p, Range r) {
    h.update_value(r.count);
    for (u32 i = 0; i < r.count; ++i) {
        TomlEntry const& e = *p.keys[r.begin + i];
        h.update_value(u32(e.key.size));
        h.update(e.key);
        h.update_value(u8(e.type));
        h.update_value(u32(e.str.size));
        h.update(e.str);
        h.update_value(e.i);
        h.update_value(e.f == 0.0 ? 0.0 : e.f);
        h.update_value(u8(e.b));
    }
}

void hash_strs(Xxh64State& h, Project const& p, Range r) {
    h.update_value(r.count);
    for (u32 i = 0; i < r.count; ++i) {
        StrView const s = p.strs[r.begin + i];
        h.update_value(u32(s.size));
        h.update(s);
    }
}

void hash_kind(Xxh64State& h, Project const& p, Kind const& k) {
    hash_range(h, p, k.defaults);
    hash_range(h, p, k.overrides);
    for (Range const& r : k.usage)
        hash_range(h, p, r);
    h.update_value(u32(k.presets.size()));
    for (Preset const& pr : k.presets) {
        h.update_value(u32(pr.name.size));
        h.update(pr.name);
        hash_range(h, p, pr.keys);
    }
    h.update_value(u32(k.rules.size()));
    for (Rule const& r : k.rules) {
        hash_strs(h, p, r.match);
        hash_strs(h, p, r.targets);
        h.update_value(r.preset);
        hash_range(h, p, r.keys);
    }
}

bool rule_matches(Project const& p, Rule const& r, StrView name, StrView target) {
    if (r.targets.count) {
        bool listed = false;
        for (u32 i = 0; i < r.targets.count && !listed; ++i)
            listed = p.strs[r.targets.begin + i] == target;
        if (!listed) return false;
    }
    for (u32 i = 0; i < r.match.count; ++i)
        if (glob_match(p.strs[r.match.begin + i], name)) return true;
    return false;
}

template <class Settings>
Status apply_range(Project const& p, Range r, Settings* s, DiagSink const* diag, SettingsTrace const* trace,
                   StrView layer, StrView file) {
    for (u32 i = 0; i < r.count; ++i) {
        TomlEntry const& e = *p.keys[r.begin + i];
        KILN_TRY(set_field(e, *s, KeyError{diag, file}));
        if (file == kOverridesName) {
            if (trace && trace->fn) trace->fn(trace->user, e.key, layer, {});
        } else {
            trace_entry(trace, e, layer, file);
        }
    }
    return kOk;
}

template <class Settings>
Status apply_defaults(Project const& p, Kind const& k, StrView kindName, Settings* s, DiagSink const* diag,
                      SettingsTrace const* trace) {
    char label[128];
    usize const n = format(label, sizeof label, "project [%.*s]", KILN_SV(kindName));
    return apply_range(p, k.defaults, s, diag, trace, StrView(label, n), p.file);
}

template <class Settings>
Status apply_rules(Project const& p, Kind const& k, CookAssetInfo const& asset, TargetProfile const& target,
                   Settings* s, DiagSink const* diag, SettingsTrace const* trace) {
    char label[128];
    for (usize i = 0; i < k.rules.size(); ++i) {
        Rule const& r = k.rules[i];
        if (!rule_matches(p, r, asset.name, target.name)) continue;
        if (r.preset >= 0) {
            Preset const& pr = k.presets[usize(r.preset)];
            usize const n =
                format(label, sizeof label, "preset %.*s (rule #%u)", KILN_SV(pr.name), unsigned(i + 1));
            KILN_TRY(apply_range(p, pr.keys, s, diag, trace, StrView(label, n), p.file));
        }
        usize const n = format(label, sizeof label, "rule #%u", unsigned(i + 1));
        KILN_TRY(apply_range(p, r.keys, s, diag, trace, StrView(label, n), p.file));
        break;
    }
    return apply_range(p, k.overrides, s, diag, trace, kOverridesName, kOverridesName);
}

} // namespace

Status apply_project_defaults(Project const& p, TextureCookSettings* s, DiagSink const* diag,
                              SettingsTrace const* trace) {
    return apply_defaults(p, p.tex, "texture", s, diag, trace);
}

Status apply_project_defaults(Project const& p, MeshCookSettings* s, DiagSink const* diag,
                              SettingsTrace const* trace) {
    return apply_defaults(p, p.mesh, "mesh", s, diag, trace);
}

Status apply_project_rules(Project const& p, CookAssetInfo const& asset, TargetProfile const& target,
                           TextureCookSettings* s, DiagSink const* diag, SettingsTrace const* trace) {
    return apply_rules(p, p.tex, asset, target, s, diag, trace);
}

Status apply_project_rules(Project const& p, CookAssetInfo const& asset, TargetProfile const& target,
                           MeshCookSettings* s, DiagSink const* diag, SettingsTrace const* trace) {
    return apply_rules(p, p.mesh, asset, target, s, diag, trace);
}

Status apply_project_usage(Project const& p, TextureCookSettings* s, u32 taken, DiagSink const* diag,
                           SettingsTrace const* trace) {
    u32 const u = u32(s->usage);
    if (u >= countof(p.tex.usage)) return kOk;
    Range const r = p.tex.usage[u];
    char label[64];
    usize const n = format(label, sizeof label, "project [texture.usage.%s]", texture_usage_name(s->usage));
    for (u32 i = 0; i < r.count; ++i) {
        TomlEntry const& e = *p.keys[r.begin + i];
        if (taken & key_bit(AssetKind::Texture, e.key)) continue;
        KILN_TRY(set_field(e, *s, KeyError{diag, p.file}));
        trace_entry(trace, e, StrView(label, n), p.file);
    }
    return kOk;
}

} // namespace detail

bool glob_match(StrView pattern, StrView name) {
    usize const pc      = pattern.find(':');
    usize const nc      = name.find(':');
    StrView const proot = pc == StrView::kNpos ? StrView() : pattern.substr(0, pc);
    StrView const nroot = nc == StrView::kNpos ? StrView() : name.substr(0, nc);
    if (proot != nroot) return false;
    return detail::glob_path(pc == StrView::kNpos ? pattern : pattern.substr(pc + 1),
                             nc == StrView::kNpos ? name : name.substr(nc + 1));
}

Result<Project*> load_project(ProjectDesc const& desc, Allocator const* alloc, DiagSink const* diag) {
    if (!alloc) alloc = default_allocator();
    Project* p = new_object<Project>(alloc, Tag::Cook, alloc);
    auto fail  = [&](Status st) -> Result<Project*> {
        delete_object(alloc, p, Tag::Cook);
        return st;
    };
    if (!desc.path.empty()) {
        p->file = p->arena.copy(desc.path);
        if (Status const st = io_read_file(compat_io_backend(), desc.path, alloc, &p->text); st.failed())
            return fail(diagf(diag, st, kDiagProjectRead, Severity::Error, desc.path, "project",
                              "cannot read the project file (%s)", code_name(st.code)));
        StrView const text(reinterpret_cast<char const*>(p->text.data()), p->text.size());
        if (Status const st =
                detail::parse_toml_subset(text, detail::TomlSyntax::Project, p->arena, p->doc, diag, p->file);
            st.failed())
            return fail(st);
        if (Status const st = detail::parse_file(*p, diag); st.failed()) return fail(st);
    }
    if (!desc.overrides.empty()) {
        p->overText.append(Span<char const>(desc.overrides.data, desc.overrides.size));
        StrView const over(p->overText.data(), p->overText.size());
        if (Status const st = detail::parse_toml_subset(over, detail::TomlSyntax::Project, p->arena, p->over,
                                                        diag, detail::kOverridesName);
            st.failed())
            return fail(st);
        if (Status const st = detail::parse_overrides(*p, diag); st.failed()) return fail(st);
    }
    Xxh64State h;
    detail::hash_kind(h, *p, p->tex);
    detail::hash_kind(h, *p, p->mesh);
    p->digest = h.digest();
    return p;
}

void free_project(Project* p) {
    if (p) delete_object(p->alloc, p, Tag::Cook);
}

Span<Root const> project_roots(Project const* p) { return p ? p->roots.span() : Span<Root const>(); }
StrView project_store(Project const* p) { return p ? p->store : StrView(); }
StrView project_target(Project const* p) { return p ? p->target : StrView(); }
u64 project_digest(Project const* p) { return p ? p->digest : 0; }

} // namespace kiln::cook
