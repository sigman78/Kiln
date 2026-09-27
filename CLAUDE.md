# kiln — agent notes

Read `docs/HANDOFF.md` first (direction, principles, milestones) and `docs/design/README.md`
(the design notes: what is decided, what awaits owner sign-off). `docs/open-questions.md` tracks anything unresolved;
add to it instead of deciding silently.

## Build

```sh
# Windows (MSVC needs the VS environment; from PowerShell or cmd):
cmd /c "call C:\dev\msvc.2026\VC\Auxiliary\Build\vcvars64.bat >nul && cmake --preset win-msvc-debug && cmake --build --preset win-msvc-debug && ctest --preset win-msvc-debug"
# clang-cl also wants that environment: use the win-clangcl-* presets the same way.
# Linux: cmake --preset linux-clang-debug   (or linux-gcc-debug)
# gcc/clang warning set on Windows: configure with -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang (GNU driver)
# Tests binary: build/<preset>/tests/kiln_tests [filter] [--list] [-v]
```

Every change must build on MSVC, clang-cl and gcc/clang (Linux) with warnings as errors.
Run `clang-format -i` on every file you touch. The pre-commit hook in `.githooks/` rejects
unformatted staged files; enable it once per clone with `git config core.hooksPath .githooks`.
Language baseline is C++23 (owner decision, 2026-09-27). Never add shims for older compilers; if a local
compiler is too old, update it.

## Hard rules (from HANDOFF §2, enforced in review)

- No `throw`/`try`/`catch`, no `dynamic_cast`/`typeid` in kiln code. Third-party code that may
  throw is caught only inside the `.cpp` that calls it and converted to `Status`.
- No `<iostream>`, `<sstream>`, `<fstream>`, `<iomanip>` anywhere. Format with `kiln::format`.
- Public headers use lightweight std headers only (`<cstdint> <cstddef> <cstring> <new>
  <type_traits> <bit> <atomic> <utility> <concepts>`); heavier ones live in `.cpp` files.
  Third-party headers never appear in `include/`.
- All allocation goes through an `Allocator` with a `Tag`. No steady-state allocation in
  `pump()`/handle polling/lookups. Arenas for per-cook / per-load temporaries.
- Recoverable errors return `Status`/`Result<T>` and emit a `Diagnostic`; broken invariants
  and API misuse use `KILN_VERIFY`/`KILN_PANIC`; `KILN_ASSERT` is debug only.
- `kiln_core` depends on nothing else in kiln. `kiln_runtime` never depends on `kiln_cook`.
- Cook-only headers go in `include/kiln/cook/`; `kiln_runtime` never includes them or writes
  files; the shipping preset must stay green.
- Declarative POD descriptor structs with defaults + designated initializers, not builders or
  long parameter lists. Function pointer + `void* user` instead of owning callables.
- Everything in `namespace kiln`; no `using namespace` in headers; `.clang-format` is law.
- Pre-1.0 API breaks are allowed but must be recorded in `CHANGELOG.md` with migration notes.
- Don't add a dependency without an entry in `docs/design/dependencies.md` and
  `third_party/README.md`. Pin versions.

## Comments

- Be critical of every comment: keep it only if it adds information the code does not show.
- A comment is for a special case, a quirk, or an in-place explanation. Anything else, and
  anything longer than ~3 lines, is documentation: move it to `docs/` and leave a one-line pointer
  at most.
- Header comments state the contract (what the caller may rely on). Source comments state
  implementation details (why this way). Do not repeat one in the other.
- The file-top comment is at most 3 lines.
- Write comments in Simple Technical English: short sentences, present tense, one idea each.
- Never refer to `docs/HANDOFF.md` or its sections in code; it is temporary and its numbering moves.
- Prune tautological comments (`// increment i`) and comments that repeat a name, a type, or a
  nearby comment.

## Layout

`include/kiln/` public headers · `src/core|io|formats|cook|runtime/` · `tests/` (own runner,
`tests/kiln_test.h`) · `tools/` (`kiln-cook`, `kiln-info`, shared parser `tools/cli.h`) ·
`examples/` (`headless`, `viewer`) · `docs/design/` design notes · `.githooks/` pre-commit hook.
