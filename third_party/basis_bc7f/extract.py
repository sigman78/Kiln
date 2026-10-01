# Extracts the bc7f encoder from Basis Universal into third_party/basis_bc7f/basisu_bc7f.cpp.
# Line numbers are pinned to commit 9bebe16726b3a61c8c213eeee3b7cffb462ef34e; every range is
# checked against an anchor text so a different checkout fails loudly.
import re, sys, pathlib

# Usage: python extract.py <basis_universal checkout at the pinned commit>
if len(sys.argv) != 2:
    sys.exit("usage: python extract.py <basis_universal checkout at the pinned commit>")
UP = pathlib.Path(sys.argv[1]) / "transcoder"
OUT = pathlib.Path(__file__).resolve().parent / "basisu_bc7f.cpp"

files = {}
def lines(name):
    if name not in files:
        raw = (UP / name).read_bytes().removeprefix(b"\xef\xbb\xbf")
        files[name] = raw.decode("utf-8", "surrogateescape").splitlines()
    return files[name]

def take(name, first, last, first_anchor, last_anchor=None):
    """1-based inclusive range; anchors are substrings of the first/last lines."""
    L = lines(name)
    if first_anchor not in L[first - 1]:
        sys.exit(f"anchor mismatch {name}:{first}: {L[first-1]!r}")
    if last_anchor is not None and last_anchor not in L[last - 1]:
        sys.exit(f"anchor mismatch {name}:{last}: {L[last-1]!r}")
    return L[first - 1:last]

def drop(block, base, ranges):
    """Removes 1-based upstream line ranges from a block that started at upstream line `base`."""
    keep = [True] * len(block)
    for a, b in ranges:
        for i in range(a, b + 1):
            keep[i - base] = False
    return [l for l, k in zip(block, keep) if k]

def resolve_zero_macros(block, macros):
    """Removes #if MACRO ... [#else ...] #endif blocks for macros that are 0, keeping the #else part."""
    out, stack = [], []  # stack entries: (is_target, in_else) or None for other conditionals
    def active():
        for e in stack:
            if e is not None and not e[1]:
                return False
        return True
    for l in block:
        s = l.strip()
        m = re.match(r"#\s*if\s+(\w+)\s*$", s)
        if m and m.group(1) in macros:
            stack.append([True, False]); continue
        if re.match(r"#\s*if", s):
            stack.append(None)
            if active(): out.append(l)
            continue
        if re.match(r"#\s*(else|elif)", s) and stack and stack[-1] is not None:
            if s.startswith("#elif"): sys.exit("elif on a resolved macro")
            stack[-1][1] = True; continue
        if re.match(r"#\s*endif", s):
            e = stack.pop()
            if e is not None: continue
            if active(): out.append(l)
            continue
        if active(): out.append(l)
    assert not stack
    return out

def remove_function(block, signature_start):
    """Removes a function (and the comment lines directly above it) whose first line starts with signature_start."""
    for i, l in enumerate(block):
        if l.startswith(signature_start):
            break
    else:
        sys.exit(f"function not found: {signature_start}")
    j = i
    while not block[j].startswith("\t{"): j += 1
    depth = 0
    k = j
    while True:
        depth += block[k].count("{") - block[k].count("}")
        if depth == 0: break
        k += 1
    s = i
    while s > 0 and block[s - 1].strip().startswith("//"): s -= 1
    e = k + 1
    if e < len(block) and block[e].strip() == "": e += 1
    return block[:s] + block[e:]

T = "basisu_transcoder.cpp"
I = "basisu_transcoder_internal.h"
U = "basisu_transcoder_uastc.h"

out = []
w = out.append
def emit(block):
    out.extend(block)

w("// basisu_bc7f.cpp: the bc7f real-time BC7 encoder of Basis Universal, extracted into one file.")
w("// Modified by the kiln project from basisu_transcoder.cpp and its headers; see NOTICE.")
lic = take(T, 2, 14, "// Copyright (C)", "// limitations under the License.")
w("//")
emit(lic)
w("")
w('#include "basisu_bc7f.h"')
w("")
w("#include <assert.h>")
w("#include <math.h>")
w("#include <string.h>")
w("")
w("#include <bit>")
w("#include <climits>")
w("#include <cmath>")
w("#include <cstdint>")
w("#include <utility>")
w("")
emit(take("basisu_containers.h", 21, 25, "#ifdef _MSC_VER", "#endif"))
emit(take("basisu.h", 70, 70, "#define BASISU_NOTE_UNUSED"))
w("")
w("namespace kiln_bc7f")
w("{")
w("namespace basisu")
w("{")
emit(take("basisu_containers.h", 33, 41, "template <typename S> inline S clamp", "minimum(S a, S b, S c, S d)"))
w("")
emit(take("basisu.h", 131, 131, "SMALL_FLOAT_VAL"))
emit(take("basisu.h", 146, 147, "squarei", "squaref"))
w("} // namespace basisu")
w("")
w("namespace basist")
w("{")
emit(take(T, 265, 265, "static inline int32_t clampi"))
w("")
emit(take(I, 221, 224, "struct bc7_block", "};"))
w("")
cr = take(I, 991, 1155, "// This duplicates key functionality", "};")
cr = drop(cr, 991, [(1011, 1011), (1014, 1030), (1046, 1050), (1149, 1153)])
emit(cr)
w("")
emit(take(T, 411, 419, "struct vec4F", "};"))
w("")
emit(take(U, 15, 15, "TOTAL_ASTC_BC7_COMMON_PARTITIONS2 = 30"))
emit(take(U, 17, 17, "TOTAL_ASTC_BC7_COMMON_PARTITIONS3 = 11"))
w("")
emit(take(U, 32, 37, "struct astc_bc7_common_partition2_desc", "};"))
w("")
emit(take(U, 50, 55, "struct astc_bc7_common_partition3_desc", "};"))
w("")
emit(take(T, 14271, 14281, "g_astc_bc7_common_partitions2[", "};"))
w("")
emit(take(T, 14292, 14297, "g_astc_bc7_common_partitions3[", "};"))
w("")
emit(take(T, 14583, 14585, "g_bc7_weights2[4]", "g_bc7_weights4[16]"))
w("")
emit(take(T, 14592, 14626, "g_bc7_partition2[64 * 16]", "};"))
w("")
emit(take(U, 174, 177, "struct endpoint_err", "};"))
w("")
e5 = take(U, 182, 183, "extern endpoint_err g_bc7_mode_5_optimal_endpoints", "BC7ENC_MODE_5_OPTIMAL_INDEX = 1")
e5[0] = e5[0].replace("extern endpoint_err", "static endpoint_err")
emit(e5)
w("")
w("\t// The BC7 777 part of transcoder_init_bc7_mode5().")
w("\tstatic void init_bc7_mode_5_optimal_endpoints()")
w("\t{")
emit(take(T, 19705, 19733, "// BC7 777", "} // c"))
w("\t}")
w("")
bc7u = take(T, 29764, 30151, "namespace bc7u", "} // namespace bc7u")
for f in ("\tint determine_bc7_mode(", "\tint determine_bc7_mode_4_index_mode(", "\tint determine_bc7_mode_4_or_5_rotation("):
    bc7u = remove_function(bc7u, f)
emit(bc7u)
w("")

body = take(T, 30157, 39480, "namespace bc7f", "} // namespace bc7f")
BASE = 30157
# Everything from pack_from_astc_4x4_single_subset() through pack_astc_6x6_to_two_subsets_middle_block(),
# and the ASTC packers and perf-stat printers after fast_pack_bc7_auto_rgb(), serve the ASTC paths only.
assert "// 4x4 ASTC blocks only" in body[32849 - BASE]
assert "#if 0" in body[33723 - BASE]
assert "void clear_perf_stats()" in body[37688 - BASE]
body = drop(body, BASE, [(32849, 33722), (37688, 39478)])
body = remove_function(body, "\tuint32_t fast_pack_bc7_auto_rgb(")
body = remove_function(body, "\tinline int fast_floorf_int(")
body = resolve_zero_macros(body, {"BASISU_BC7F_PERF_STATS", "BASISU_BC7F_USE_SSE41"})
text = "\n".join(body)
# popcount via <bit> instead of compiler intrinsics (__popcnt needs <intrin.h> and a POPCNT CPU).
text = text.replace("""\tstatic int popcount32(uint32_t x)
\t{
#if defined(__EMSCRIPTEN__) || defined(__clang__) || defined(__GNUC__)
\t\treturn __builtin_popcount(x);
#elif defined(_MSC_VER)
\t\treturn __popcnt(x);
#else
\t\tint count = 0;
\t\twhile (x)
\t\t{
\t\t\tx &= (x - 1);
\t\t\t++count;
\t\t}
\t\treturn count;
#endif
\t}""", """\tstatic int popcount32(uint32_t x)
\t{
\t\treturn std::popcount(x);
\t}""", 1)
text = text.replace("""\tstatic inline int pop16(uint32_t x)
\t{
#if defined(_MSC_VER)
\t\treturn __popcnt16((unsigned short)x);
#else
\t\treturn __builtin_popcount(x & 0xFFFFu);
#endif
\t}""", """\tstatic inline int pop16(uint32_t x)
\t{
\t\treturn std::popcount(x & 0xFFFFu);
\t}""", 1)
assert "__popcnt" not in text and "__builtin" not in text and "_mm_" not in text
# The public init() also fills the mode 5 table, which basisu_transcoder_init() fills upstream.
text = text.replace("\tvoid init()\n\t{\n", "\tvoid init()\n\t{\n\t\tinit_bc7_mode_5_optimal_endpoints();\n\n", 1)
out.extend(text.split("\n"))
w("")
w("} // namespace basist")
w("")
w("void init()")
w("{")
w("\tbasist::bc7f::init();")
w("}")
w("")
w("uint32_t fast_pack_bc7_auto_rgba(uint8_t block[16], const uint8_t rgba[64], uint32_t flags)")
w("{")
w("\t// Upstream reads texels as uint32_t.")
w("\talignas(4) basist::color_rgba pixels[16];")
w("\tmemcpy(pixels, rgba, sizeof(pixels));")
w("\treturn basist::bc7f::fast_pack_bc7_auto_rgba(block, pixels, flags);")
w("}")
w("")
w("} // namespace kiln_bc7f")
w("")

src = "\n".join(out)
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_bytes(src.encode("utf-8", "surrogateescape"))
print(OUT, src.count("\n"), "lines")
