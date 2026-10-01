// fuzz/fuzz_toml_subset.cpp — libFuzzer target: kiln's TOML subset parser (sidecars and kiln.toml)
// on arbitrary bytes, in both syntaxes; a parsed document must be self-consistent.
#include "../src/cook/toml_subset.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    using namespace kiln;
    using namespace kiln::cook::detail;
    StrView const text(reinterpret_cast<char const*>(data), size);
    for (TomlSyntax const syntax : {TomlSyntax::Sidecar, TomlSyntax::Project}) {
        Arena arena(Arena::Desc{default_allocator(), 4096, Tag::Cook});
        TomlDoc doc(default_allocator());
        if (parse_toml_subset(text, syntax, arena, doc, nullptr, "fuzz").failed()) continue;
        if (doc.tables.empty() || !doc.tables[0].name.empty()) __builtin_trap();
        for (TomlEntry const& e : doc.entries) {
            if (e.table >= doc.tables.size() || e.section != doc.tables[e.table].name) __builtin_trap();
            if (e.type != TomlType::Array && e.items.size != 0) __builtin_trap();
            if (syntax == TomlSyntax::Sidecar && e.type == TomlType::Array) __builtin_trap();
            for (TomlValue const& v : e.items)
                if (v.type != e.items[0].type || v.type == TomlType::Array) __builtin_trap();
        }
    }
    return 0;
}
