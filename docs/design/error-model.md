# Error model

**Status:** Proposed (awaiting owner sign-off)
**Milestone:** M0
**Decides:** How kiln reports recoverable errors (Status, Result, diagnostics) and when it panics instead.

## Decision

This note describes what is already implemented in `include/kiln/result.h` and
`include/kiln/core.h`, plus two proposals: the diagnostic code namespace and the third-party
exception boundary.

### Two classes of error

| Class | Mechanism | Examples |
|---|---|---|
| **Recoverable** | return `Status` / `Result<T>`, optionally emit a `Diagnostic` | IO failure, parse error, validation failure, unsupported format, adapter back-pressure |
| **Non-recoverable** | panic | broken invariant, API misuse, allocator out-of-memory |

Panic macros (`core.h`):

| Macro | When active | Use for |
|---|---|---|
| `KILN_PANIC(fmt, ...)` | always | unreachable states, fatal misuse |
| `KILN_VERIFY(cond)` | always | API contracts and invariants that must hold in release |
| `KILN_ASSERT(cond)` | only when `KILN_DEBUG` (defaults to `!NDEBUG`) | internal invariants, bounds checks in containers |

`panic()` formats the message and calls the handler set by `set_panic_handler(fn, user)`. The
default handler logs to the log sink (category `panic`, or stderr if no sink), breaks into the
debugger and aborts. If a user handler returns, `panic()` still aborts.

Allocator out-of-memory panics: `kiln::alloc()` panics on null. `try_alloc()` exists for the rare
caller that can recover, which then returns `Code::OutOfMemory`.

**Runtime API misuse that panics** (always on, `KILN_VERIFY` / `KILN_PANIC`, with a message that
names the rule):

| Misuse | Why a panic, not a `Status` |
|---|---|
| `wait()` called off the pump thread | `pump()` state is single-threaded; running it from two threads would race |
| `wait()` with an adapter that lacks `AdapterCaps::kSelfSubmitting` | uploads cannot complete without the host recording a frame, so the call would hang until the timeout on every use |
| `pump()` called from a thread other than the bound pump thread (proposed, see `handles-and-states.md`) | same race as above |

Rule: a blocking API never hangs on misuse. It panics at entry, before blocking.

### `Code`

`enum class Code : u16`. Stable once released, append only. `code_name(Code)` returns a
snake_case name.

| Code | Used when |
|---|---|
| `Ok` | success |
| `Unknown` | unclassified failure; avoid, prefer a specific code |
| `InvalidArgument` | caller passed a value the API rejects (recoverable form; contract violations panic instead) |
| `OutOfMemory` | an allocation failed where the caller opted in to recover via `try_alloc` |
| `NotFound` | asset, file, key or store entry does not exist |
| `AlreadyExists` | registering an id that is already registered; store entry exists |
| `Unsupported` | format, extension or feature not supported (Draco, sparse accessors, BCn in v0.5) |
| `IoError` | OS-level IO failure; `detail` carries the OS error |
| `IoEof` | read past end of file (truncated file) |
| `ParseError` | malformed input: glTF JSON, PNG, KTX2 header, config |
| `ValidationFailed` | well-formed input that breaks a semantic rule (index out of range, bad settings combination) |
| `Corrupt` | cooked data failed integrity checks (bad `.mesh` section bounds, `BLOB` table ranges or overlaps, decoded size mismatch, store entry mismatch) |
| `VersionMismatch` | cooked data from an incompatible `.mesh` major version or cooker version |
| `Busy` | back-pressure: adapter or budget says "not now", retry next `pump()` |
| `NotReady` | the target is not in a state that allows the operation |
| `Cancelled` | request released or context destroyed while work was in flight |
| `Timeout` | a bounded wait expired |
| `Internal` | an invariant failure caught at a boundary, such as a third-party exception |

### `Status`

```cpp
struct Status {
    Code code   = Code::Ok;
    u16  detail = 0;   // code-specific
};
inline constexpr Status kOk{};
constexpr Status make_status(Code c, u16 detail = 0);
```

- 4 bytes, trivially copyable, returned in a register.
- `detail` meaning:
  - `IoError`: `errno`, or the low 16 bits of `GetLastError()` on Windows.
  - Other codes: code-specific, documented at the emitting site. For example, a parse error may
    carry the failing section fourcc index or a small sub-reason. 0 when unused.
- **No string payload, by design.** A string needs either heap allocation or an ownership rule
  (static? arena? who frees?). Both are wrong for a value that is copied and returned through
  every layer. Human-readable context goes to a `DiagSink` at the point where it is known.

### `Result<T>`

- Value-or-Status. Inline storage (`alignas(T) unsigned char[sizeof(T)]`) plus a `Status`. No heap.
- Constructed implicitly from `T` (success) or from `Status` / `Code` (failure). Constructing from
  an Ok status asserts.
- `value()`, `operator*`, `operator->` check `ok()` with `KILN_ASSERT`. Debug builds panic on
  misuse; release builds do not check.
- `value_or(fallback)`, `status()`, `code()`, `ok()`, `failed()`.
- `Result<T&>` is rejected by `static_assert`; use `Result<T*>`.
- `Result<void>` carries only a `Status` and default-constructs to Ok.

### Propagation macros

```cpp
KILN_TRY(read_header(src, &hdr));           // returns the Status on failure
KILN_TRY_ASSIGN(auto blob, load_blob(src)); // binds the value on success
```

Both accept a `Status` or any `Result<T>`, and return a `Status` from the enclosing function.
The enclosing function must therefore return `Status` or a `Result<U>`.

### Diagnostics

```cpp
enum class Severity : u8 { Info, Warning, Error };

struct Diagnostic {
    u32      code     = 0;       // stable diagnostic code, 0 = none (see namespace below)
    Severity severity = Severity::Error;
    Status   status   = kOk;     // the Status this diagnostic accompanies, if any
    StrView  asset;              // asset path or id text
    StrView  where;              // node, material, section, ...
    StrView  message;            // human-readable
};

struct DiagSink { void (*fn)(void* user, Diagnostic const&); void* user; };
```

- All views are valid only during the callback. The sink copies what it keeps.
- A null sink or null `fn` drops diagnostics. `emit(sink, d)` handles both.
- `diagf(sink, status, code, severity, asset, where, fmt, ...)` formats into a stack buffer
  (`kLogMessageMax` = 1024 bytes) and returns `status`, so the idiom is
  `return diagf(diag, make_status(Code::ParseError), 1003, Severity::Error, path, node, "...");`.
- `log_diag_sink()` forwards to the log (category `diag`), printing `K%04u` when a code is set.
- The sink must be thread-safe if it is shared by concurrent cooks. Inside the runtime,
  diagnostics from workers are queued and delivered on the pumping thread (see
  `threading-and-io.md`).

### Runtime contract for recoverable errors

When loading an asset fails for a recoverable reason:

1. The asset moves to `Failed` (see `handles-and-states.md`).
2. Textures: `gpu()` serves the Failed placeholder (magenta checker when `devPlaceholders` is on,
   else the kind placeholder). Meshes have no placeholder: `is_ready()` stays false and `view()`
   is empty.
3. **Exactly one** diagnostic with `Severity::Error` is emitted for the failure, plus a `Failed`
   event on `pump()`. Warnings emitted earlier during the same cook are not limited.

### `.mesh` blob table failures

The blob decode loop (mesh-format-spec §5.9, §7) fails the asset recoverably. Proposed mapping:

| Failure | `Code` | Diagnostic |
|---|---|---|
| Blob encoded range outside `GPUD`, or decoded range outside `payloadDecodedSize` | `Corrupt` | K4xxx |
| Decoded ranges overlap, or do not cover a range that `LODS` references | `Corrupt` | K4xxx |
| Decoder output size differs from `decodedSize` (short, or would write past it) | `Corrupt` | K4xxx |
| `kPayloadRaw` set but a blob is not an identity range (codec, filter, offsets or sizes differ) | `Corrupt` | K4xxx |
| `checksum` mismatch (tools and debug builds) | `Corrupt` | K4xxx |
| Misaligned offsets, `elementSize` 0 where required, `decodedSize` not a multiple of `elementSize` | `ValidationFailed` | K4xxx |
| Unknown codec or filter id, or one not built into this runtime (anything but `None` in v0.5) | `Unsupported` | K4xxx |

- `Corrupt` means the bytes cannot be trusted (bounds, overlaps, sizes). `ValidationFailed` means
  the file is structurally sound but breaks a spec rule. Both are recoverable: the asset goes to
  `Failed`.
- `Status.detail` carries the blob index (low 16 bits) so tools can point at the entry.
- The same checks run in the cooker's writer self-check (K4xxx, `Severity::Error`) and in
  `kiln-info`.

Never a crash, never a silent failure (v0.5 exit criterion).

### Proposal: diagnostic code namespace

- Codes are `u32`, rendered as `K` plus 4 digits (`K1003`). 0 means "no code".
- Codes are stable once released. Each code is documented in a future `docs/diagnostics.md`
  (created in M2) with a one-line meaning and an artist-facing fix hint.
- Ranges:

| Range | Area |
|---|---|
| K1000-1999 | glTF import (unsupported extensions, sparse accessors, Draco, bad node names, missing UVs) |
| K2000-2999 | image import and encode (PNG decode, KTX2 pass-through, size, channel checks) |
| K3000-3999 | settings resolution (invalid combinations, unknown keys later) |
| K4000-4999 | `.mesh` and KTX2 validation (reader and writer checks, including `BLOB` table checks) |
| K5000-5999 | runtime and store (store miss, corrupt entry, adapter failures, placeholder served) |
| K6000-9999 | reserved |

- Codes are defined as `constexpr u32` in the emitting module's private header, and the doc table
  is the source of truth. A test checks there are no duplicates (M2).

### Proposal: exception boundary for third-party code

- kiln code never uses `throw`, `try`, `catch`, `dynamic_cast` or `typeid`.
- If a dependency can throw on bad input, the **only** `try`/`catch` allowed is in the single
  `.cpp` that wraps it. It catches, converts to `Status` (usually `ParseError` or `Internal`),
  and emits a diagnostic.
- When `KILN_NO_EXCEPTIONS` lands (v1.0), those blocks are compiled out behind the macro.
- None of the v0.5 dependencies in `dependencies.md` throw, so no such block is expected in v0.5.

## Rationale

- A 4-byte `Status` is free to return everywhere and keeps `Result<T>` small.
- Separating "what failed" (Status) from "why, in words" (Diagnostic) keeps the hot path
  allocation-free and gives tools structured, documentable output.
- Panicking on misuse keeps API contracts strict without burdening every call with checks the
  caller cannot act on.

## Alternatives considered

- **`std::expected`**: C++23, not guaranteed on all targets; pulls a heavier header.
- **Status with an owned message string**: heap in the error path, ownership questions.
- **Status with a `char const*` to a static string**: forces messages to be static, loses context
  (paths, node names).
- **Error codes as a global registry**: needs a registration step and a global; a plain `enum`
  plus module-local diagnostic codes is simpler.

## Consequences / what this constrains later

- `Code` is append-only after the first release. Reordering is an ABI and log-format break.
- Diagnostic code numbers become part of the artist-facing documentation; renumbering is not
  allowed after release.
- Any API that can fail recoverably returns `Status` or `Result<T>`; no out-parameter error codes.

## Open points for the owner

- Confirm the `K` + 4-digit code format and the ranges above.
- Confirm "exactly one Error diagnostic per failed load".
- Confirm that `Status.detail` stays 16 bits (it truncates Windows error codes to their low bits).
- Confirm the `wait()` misuse panics (off-thread, adapter without `kSelfSubmitting`).
- Confirm the `Corrupt` vs `ValidationFailed` split for blob-table failures above.
