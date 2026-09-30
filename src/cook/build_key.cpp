// Build keys: XXH3-128 over a tagged, little-endian serialization, never struct memory.
#include "kiln/containers.h"
#include "kiln/cook/cook.h"
#include "kiln/cook/manifest.h"

namespace kiln::cook {

namespace {

// Field tags. A new field takes a new tag; a changed meaning bumps kBuildKeySchema.
enum : u8 {
    kTagSchema   = 1,
    kTagCooker   = 2,
    kTagKind     = 3,
    kTagName     = 4,
    kTagTarget   = 5,
    kTagSettings = 6,
    kTagInput    = 7,
};

struct Writer {
    Vec<u8> out{default_allocator(), Tag::Cook};

    void le(u64 v, usize bytes) noexcept {
        for (usize i = 0; i < bytes; ++i)
            out.push_back(u8(v >> (8 * i)));
    }
    void str(StrView s) noexcept {
        le(s.size, 4);
        out.append(Span<u8 const>(reinterpret_cast<u8 const*>(s.data), s.size));
    }
};

} // namespace

Hash128 build_key(BuildKeyDesc const& d) noexcept {
    Writer w;
    w.out.reserve(64 + d.name.size + d.inputs.size * 48);
    w.le(kTagSchema, 1);
    w.le(kBuildKeySchema, 4);
    w.le(kTagCooker, 1);
    w.le(kCookerVersion, 4);
    w.le(kTagKind, 1);
    w.le(u8(d.kind), 1);
    w.le(kTagName, 1);
    w.str(d.name);
    w.le(kTagTarget, 1);
    w.le(d.targetHash, 8);
    w.le(kTagSettings, 1);
    w.le(d.settingsHash, 8);
    for (BuildInput const& in : d.inputs) {
        w.le(kTagInput, 1);
        w.le(u8(in.role), 1);
        w.str(in.name);
        w.out.append(Span<u8 const>(in.content.bytes, sizeof in.content.bytes));
    }
    return xxh3_128(w.out.span());
}

} // namespace kiln::cook
