// src/runtime/names.cpp — asset name rules, URI resolution, the Named store layout, shape names.
// The rules: docs/design/asset-model-next.md, Part 2.
#include "kiln/assets.h"

#include "kiln/log.h"

#include <cstring>

namespace kiln {

namespace {

[[nodiscard]] bool is_root_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/// Checks the part after the root prefix, `#sub` included.
char const* check_path(StrView path) {
    usize const hash  = path.find('#');
    usize const slash = path.rfind('/');
    if (hash != StrView::kNpos) {
        if (path.find('#', hash + 1) != StrView::kNpos) return "more than one '#'";
        if (slash != StrView::kNpos && slash > hash) return "'#' before the last '/'";
        if (hash + 1 == path.size) return "empty name after '#'";
        if (hash == (slash == StrView::kNpos ? 0 : slash + 1)) return "no file name before '#'";
    }
    usize segBegin = 0;
    for (usize i = 0; i <= path.size; ++i) {
        if (i < path.size) {
            char const c = path[i];
            if (u8(c) < 0x20 || c == 0x7f) return "control character";
            if (c == '\\') return "'\\' (use '/')";
            if (c == ':') return "':' outside the root prefix";
            if (c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*')
                return "character not allowed in Windows file names";
            if (c != '/') continue;
        }
        StrView const seg = path.substr(segBegin, i - segBegin);
        if (seg.empty()) return "empty segment (leading, trailing or double '/')";
        if (seg == "." || seg == "..") return "'.' or '..' segment";
        segBegin = i + 1;
    }
    return nullptr;
}

} // namespace

char const* texture_shape_name(TextureShape s) {
    switch (s) {
    case TextureShape::Tex2D: return "2D";
    case TextureShape::Cube: return "cube";
    case TextureShape::Array: return "array";
    case TextureShape::Count: break;
    }
    return "unsupported";
}

AssetNameParts split_asset_name(StrView name) {
    AssetNameParts p;
    usize const colon = name.find(':');
    if (colon != StrView::kNpos) {
        p.root = name.substr(0, colon);
        name   = name.substr(colon + 1);
    }
    usize const hash = name.find('#');
    p.path           = hash == StrView::kNpos ? name : name.substr(0, hash);
    if (hash != StrView::kNpos) p.sub = name.substr(hash + 1);
    return p;
}

char const* check_root_name(StrView root) {
    if (root.size < 2) return "root name shorter than 2 characters";
    if (root == "default") return "'default' is reserved: it names the default root in kiln.toml";
    for (char const c : root)
        if (!is_root_char(c)) return "root name outside [a-z0-9_]";
    return nullptr;
}

char const* check_asset_name(StrView name) {
    if (name.empty()) return "empty name";
    if (name.size > kMaxAssetNameLen) return "longer than 255 bytes";
    usize const colon = name.find(':');
    if (colon != StrView::kNpos) {
        if (char const* why = check_root_name(name.substr(0, colon))) return why;
        name = name.substr(colon + 1);
        if (name.empty()) return "empty path after the root";
    } else if (name[0] == '@') {
        // The store writes named root `m:` as the top-level directory `@m/`.
        return "a default-root path may not start with '@'";
    }
    return check_path(name);
}

usize resolve_asset_name(StrView owner, StrView uri, char* out, usize cap) {
    if (cap == 0 || check_asset_name(owner)) return 0;
    if (uri.empty() || uri[0] == '/' || uri.find(':') != StrView::kNpos || uri.find('\\') != StrView::kNpos)
        return 0;
    AssetNameParts const o = split_asset_name(owner);

    usize n = 0;
    if (!o.root.empty()) {
        n = format(out, cap, "%.*s:", KILN_SV(o.root));
        if (n >= cap - 1) return 0;
    }
    usize const pathBegin = n;
    usize const slash     = o.path.rfind('/');
    if (slash != StrView::kNpos) {
        n += format(out + n, cap - n, "%.*s", KILN_SV(o.path.substr(0, slash)));
        if (n >= cap - 1) return 0;
    }
    for (usize at = 0; at <= uri.size;) {
        usize end = uri.find('/', at);
        if (end == StrView::kNpos) end = uri.size;
        StrView const seg = uri.substr(at, end - at);
        at                = end + 1;
        if (seg == ".") continue;
        if (seg == "..") {
            if (n == pathBegin) return 0; // leaves the root
            while (n > pathBegin && out[n - 1] != '/')
                --n;
            if (n > pathBegin) --n; // the separator itself
            continue;
        }
        if (seg.empty()) return 0;
        n += format(out + n, cap - n, n > pathBegin ? "/%.*s" : "%.*s", KILN_SV(seg));
        if (n >= cap - 1) return 0;
    }
    return check_asset_name(StrView(out, n)) ? 0 : n;
}

StrView texture_asset_name(StrView meshName, mesh::MeshView const& v, mesh::TextureBinding const& b,
                           char* out, usize cap) {
    StrView const path = v.str(b.pathStr);
    if (b.flags & mesh::kTextureExternal) return StrView(out, resolve_asset_name(meshName, path, out, cap));
    if (path.size >= cap || check_asset_name(path)) return {};
    std::memcpy(out, path.data, path.size);
    out[path.size] = '\0';
    return StrView(out, path.size);
}

} // namespace kiln
