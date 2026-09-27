# Third-party dependencies

| Dependency | Version/commit | License | Used by | Notes |
|---|---|---|---|---|
| None yet. Candidates under discussion: see [`docs/design/dependencies.md`](../docs/design/dependencies.md). | | | | |

## Rules for adding a dependency

- **Pin versions.** Use `FetchContent` with a commit hash, or vendor the source directly. Never
  float on a branch or tag alone.
- **Build with our flags.** Dependencies are built with the same compiler flags as the rest of the
  project where possible (see `cmake/kiln_warnings.cmake`), not their own defaults.
- **Permissive license only.** MIT, BSD, zlib, Apache-2.0, or a similarly permissive license.
  Nothing copyleft.
- **Record it here.** Every dependency gets a row in the table above with its license and which
  kiln target(s) use it.
