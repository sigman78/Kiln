# kiln — agent notes

Read `docs/HANDOFF.md` first (direction, principles, milestones) and `docs/design/README.md`
(M0 design notes awaiting owner sign-off). `docs/open-questions.md` tracks anything unresolved;
add to it instead of deciding silently.

## Build

```sh
# Windows (MSVC needs the VS environment; from PowerShell or cmd):
cmd /c "call C:\dev\msvc.2026\VC\Auxiliary\Build\vcvars64.bat >nul && cmake --preset win-msvc-debug && cmake --build --preset win-msvc-debug && ctest --preset win-msvc-debug"
# clang-cl also wants that environment: use the win-clangcl-* presets the same way.
# Linux: cmake --preset linux-clang-debug   (or linux-gcc-debug)
# Tests binary: build/<preset>/tests/kiln_tests [filter] [--list] [-v]
```

Every change must build on MSVC, clang-cl and gcc/clang (Linux) with warnings as errors.

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

## Layout

`include/kiln/` public headers · `src/core|io|formats|cook|runtime/` · `tests/` (own runner,
`tests/kiln_test.h`) · `tools/` (M1+) · `examples/` (M3+) · `docs/design/` design notes.
