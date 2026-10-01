// tests/test_toml_subset.cpp — the project-file forms of kiln's TOML subset (src/cook/toml_subset.h):
// arrays, arrays of tables, quoted header segments, and the TOML rules they bring.
#include "kiln_test.h"

#include "../src/cook/toml_subset.h"

#include "kiln/cook/settings.h"

using namespace kiln;
using namespace kiln::cook;
using namespace kiln::cook::detail;

namespace {

struct Parsed {
    Arena arena{
        Arena::Desc{default_allocator(), 4096, Tag::Test}
    };
    TomlDoc doc{default_allocator()};
    u32 code = 0;
    char where[64]{};

    static void fn(void* user, Diagnostic const& d) {
        auto* self = static_cast<Parsed*>(user);
        self->code = d.code;
        format(self->where, sizeof self->where, "%.*s", KILN_SV(d.where));
    }

    Status parse(StrView text, TomlSyntax syntax = TomlSyntax::Project) {
        DiagSink const sink{&fn, this};
        return parse_toml_subset(text, syntax, arena, doc, &sink, "kiln.toml");
    }
    TomlEntry const* find(u32 table, StrView key) const {
        for (TomlEntry const& e : doc.entries)
            if (e.table == table && e.key == key) return &e;
        return nullptr;
    }
};

} // namespace

KILN_TEST(TomlSubset, Arrays) {
    Parsed p;
    KILN_REQUIRE(p.parse("one = [\"a\", 'b']\n"
                         "ints = [1, -2, +3,]\n"
                         "empty = []\n"
                         "multi = [ # comment\n"
                         "  1.5,\r\n"
                         "\n"
                         "  2e1, # another\n"
                         "]\n"
                         "after = true\n")
                     .ok());
    TomlEntry const* one = p.find(0, "one");
    KILN_REQUIRE(one && one->type == TomlType::Array && one->items.size == 2);
    KILN_CHECK(one->items[0].type == TomlType::String && one->items[0].str == "a");
    KILN_CHECK(one->items[1].str == "b");
    TomlEntry const* ints = p.find(0, "ints");
    KILN_REQUIRE(ints && ints->items.size == 3);
    KILN_CHECK_EQ(ints->items[1].i, i64(-2));
    KILN_CHECK_EQ(ints->items[2].i, i64(3));
    TomlEntry const* empty = p.find(0, "empty");
    KILN_REQUIRE(empty && empty->type == TomlType::Array);
    KILN_CHECK_EQ(empty->items.size, usize(0));
    TomlEntry const* multi = p.find(0, "multi");
    KILN_REQUIRE(multi && multi->items.size == 2);
    KILN_CHECK_EQ(multi->line, 4u);
    KILN_CHECK(multi->items[1].type == TomlType::Float && multi->items[1].f == 20.0);
    TomlEntry const* after = p.find(0, "after");
    KILN_REQUIRE(after);
    KILN_CHECK_EQ(after->line, 9u); // lines inside the array still count
}

KILN_TEST(TomlSubset, ArraysOfTablesAndHeaders) {
    Parsed p;
    KILN_REQUIRE(p.parse("[texture]\n"
                         "quality = \"high\"\n"
                         "[texture.preset.ui]\n"
                         "genMips = false\n"
                         "[texture.preset.\"ui 2\"]\n"
                         "[[texture.rule]]\n"
                         "match = [\"ui/**\"]\n"
                         "preset = \"ui\"\n"
                         "[[texture.rule]]   # second\n"
                         "match = [\"**\"]\n"
                         "[a.b]\n"
                         "[a]\n" // a super-table may be defined after its sub-table
                         "x = 1\n")
                     .ok());
    KILN_REQUIRE_EQ(p.doc.tables.size(), usize(8));
    KILN_CHECK(p.doc.tables[0].name.empty());
    KILN_CHECK(p.doc.tables[2].name == "texture.preset.ui");
    KILN_CHECK(p.doc.tables[3].name == "texture.preset.ui 2");
    KILN_CHECK(p.doc.tables[4].array && p.doc.tables[5].array);
    KILN_CHECK(p.doc.tables[4].name == "texture.rule");
    KILN_CHECK_EQ(p.doc.tables[5].line, 9u);
    // The same key in two elements of an array of tables is fine; each element is its own table.
    TomlEntry const* m1 = p.find(4, "match");
    TomlEntry const* m2 = p.find(5, "match");
    KILN_REQUIRE(m1 && m2);
    KILN_CHECK(m1->items[0].str == "ui/**");
    KILN_CHECK(m2->items[0].str == "**");
    KILN_CHECK(m1->section == "texture.rule");
    KILN_CHECK(p.find(4, "preset") != nullptr);
    KILN_CHECK(p.find(5, "preset") == nullptr);
    KILN_CHECK(p.doc.tables[7].name == "a");
    KILN_CHECK(p.find(7, "x") != nullptr);
}

KILN_TEST(TomlSubset, OutsideTheSubsetOrInvalidTomlFails) {
    char const* const bad[] = {
        "a = [1, \"x\"]",      // mixed types
        "a = [[1], [2]]",      // nested arrays
        "a = [{b = 1}]",       // inline table in an array
        "a = {b = 1}",         // inline table
        "a = [1 2]",           // missing comma
        "a = [1,",             // unterminated
        "a = [,]",             // a comma without a value
        "a = [\"x\n\"]",       // a string across lines
        "a.b = 1",             // dotted key
        "\"a\" = 1",           // quoted key
        "a = \"\"\"x\"\"\"",   // multi-line string
        "a = 1 # \x01",        // control character in a comment
        "[a]\n[a]",            // table twice
        "[a]\n[[a]]",          // table, then array of tables
        "[[a]]\n[a]",          // array of tables, then table
        "[[a.b]]\n[[a]]",      // the array would replace the implicit table a
        "[a.b]\n[[a]]",        //
        "[[a]]\n[a.b]",        // tables inside an array of tables
        "[[a]]\n[[a.b]]",      //
        "a = 1\n[a]",          // key, then table of that name
        "[x]\na = 1\n[x.a.b]", // key, then a table under it
        "[a.b]\n[a]\nb = 1",   // table, then key of that name
        "[\"a.b\"]",           // quoted segment with '.'
        "[\"\"]",              // empty quoted segment
        "[[a] ]",              // ']]' split
        "[[a]] x = 1",         // text after the header
        "a = [1, 2] x",        // text after an array
        "[[a]]\nk = 1\nk = 2", // key twice in one element
    };
    for (char const* text : bad) {
        Parsed p;
        Status const st = p.parse(StrView(text));
        KILN_CHECK_MSG(st.code == Code::ParseError, "accepted: %s", text);
        KILN_CHECK_MSG(p.code == kDiagSidecarSyntax, "%s: code %u", text, p.code);
    }
}

KILN_TEST(TomlSubset, ErrorLineAfterAMultiLineArray) {
    Parsed p;
    KILN_CHECK(p.parse("a = [\n  1,\n  2,\n]\nb = oops\n").failed());
    KILN_CHECK(StrView(p.where) == "kiln.toml:5");
    Parsed q;
    KILN_CHECK(q.parse("[x]\nk = 1\n\n[y.k]\n[x.k]\n").failed()); // reported at the key
    KILN_CHECK(StrView(q.where) == "kiln.toml:2");
}

KILN_TEST(TomlSubset, SidecarKeepsItsSubset) {
    char const* const projectOnly[] = {"a = [1]", "[[a]]", "[\"a\"]"};
    for (char const* text : projectOnly) {
        Parsed p;
        KILN_CHECK_MSG(p.parse(StrView(text), TomlSyntax::Sidecar).code == Code::ParseError, "accepted: %s",
                       text);
        Parsed q;
        KILN_CHECK_MSG(q.parse(StrView(text), TomlSyntax::Project).ok(), "rejected: %s", text);
    }
}
