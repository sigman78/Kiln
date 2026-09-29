# Error model

**Status:** Proposed (awaiting owner sign-off). Implemented: `include/kiln/result.h`,
`include/kiln/core.h`, `src/core/`; the code catalogue is `../diagnostics.md`.
**Decides:** How kiln reports recoverable errors (Status, Result, diagnostics) and when it panics
instead.

## Decision

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

Out of memory: `kiln::alloc()` panics on null. `try_alloc()` is for the rare caller that can
recover, which then returns `Code::OutOfMemory`.

**Runtime API misuse:**

| Misuse | Handling | Why |
|---|---|---|
| `wait()` called off the pump thread | panic, K5007 | `pump()` state is single-threaded; two threads would race |
| `wait()` with an adapter that lacks `kSelfSubmitting` | panic, K5007 | uploads cannot complete without the host recording a frame, so the call would hang until the timeout |
| `pump()` called off the pump thread | `KILN_ASSERT`, debug builds only (R5i) | same race as above |

Rule: a blocking API never hangs on misuse. It panics at entry, before blocking.

### `Code`

`enum class Code : u16`, append only once released. `code_name(Code)` returns a snake_case name.

| Code | Used when |
|---|---|
| `Ok` | success |
| `Unknown` | unclassified failure; avoid, prefer a specific code |
| `InvalidArgument` | caller passed a value the API rejects (recoverable form; contract violations panic instead) |
| `OutOfMemory` | an allocation failed where the caller opted in to recover via `try_alloc` |
| `NotFound` | asset, file, key or store entry does not exist |
| `AlreadyExists` | registering a path that is already registered; store entry exists |
| `Unsupported` | format, extension or feature not supported (Draco, sparse accessors, compressed `.mesh` codecs in v0.5) |
| `IoError` | OS-level IO failure; `detail` carries the OS error |
| `IoEof` | read past end of file (truncated file) |
| `ParseError` | malformed input: glTF JSON, PNG/JPEG/WebP, KTX2 header, config |
| `ValidationFailed` | well-formed input that breaks a semantic rule (index out of range, bad settings combination) |
| `Corrupt` | cooked data failed integrity checks (section bounds, `BLOB` ranges or overlaps, decoded size mismatch, store entry mismatch) |
| `VersionMismatch` | cooked data from an incompatible `.mesh` version (other major, or other minor while the major is 0) or cooker version |
| `Busy` | back-pressure: adapter, budget or table says "not now" |
| `NotReady` | the target is not in a state that allows the operation |
| `Cancelled` | request released or context destroyed while work was in flight |
| `Timeout` | a bounded wait expired |
| `Internal` | an invariant failure caught at a boundary, such as a third-party exception |

### `Status`

`struct Status { Code code; u16 detail; }`, `kOk`, `make_status(Code, u16 detail = 0)`.

- 4 bytes, trivially copyable, returned in a register.
- `detail`: for `IoError`, `errno` or the low 16 bits of `GetLastError()`; for other codes,
  code-specific and documented at the emitting site; 0 when unused.
- **No string payload, by design.** A string needs heap allocation or an ownership rule, both wrong
  for a value copied through every layer. Human-readable context goes to a `DiagSink` where it is
  known.

### `Result<T>`

- Value-or-Status with inline storage plus a `Status`. No heap.
- Constructed implicitly from `T` (success) or from `Status` / `Code` (failure). Constructing from
  an Ok status asserts.
- `value()`, `operator*`, `operator->` check `ok()` with `KILN_ASSERT`: debug builds panic on
  misuse, release builds do not check.
- `value_or(fallback)`, `status()`, `code()`, `ok()`, `failed()`.
- `Result<T&>` is rejected by `static_assert`; use `Result<T*>`. `Result<void>` carries only a
  `Status` and default-constructs to Ok.
- `KILN_TRY(expr)` returns the `Status` on failure; `KILN_TRY_ASSIGN(decl, expr)` also binds the
  value on success. Both accept a `Status` or any `Result<T>`, so the enclosing function returns
  `Status` or a `Result<U>`.

### Diagnostics

`Diagnostic { u32 code; Severity severity; Status status; StrView asset, where, message; }` and
`DiagSink { fn, user }`, with `Severity { Info, Warning, Error }`.

- All views are valid only during the callback. The sink copies what it keeps.
- A null sink or null `fn` drops diagnostics; `emit(sink, d)` handles both.
- `diagf(sink, status, code, severity, asset, where, fmt, ...)` formats into a stack buffer
  (`kLogMessageMax` = 1024 bytes) and returns `status`, so the idiom is
  `return diagf(diag, make_status(Code::ParseError), 1003, Severity::Error, path, node, "...");`.
- `log_diag_sink()` forwards to the log (category `diag`), printing `K%04u` when a code is set.
- A sink shared by concurrent cooks must be thread-safe. The runtime never calls the host's sink
  from a worker (`threading-and-io.md`).

### Runtime contract for recoverable errors

When loading an asset fails for a recoverable reason:

1. The asset moves to `Failed` (`handles-and-states.md`).
2. Textures: `gpu_object()` serves the Failed placeholder (magenta checker when `devPlaceholders` is on,
   else the kind placeholder). Meshes: `is_ready()` stays false and `mesh_view()` is null.
3. **Exactly one** `Severity::Error` diagnostic (K5xxx) is emitted, plus a `Failed` event, in
   `pump()`. It carries the first diagnostic the worker produced (reader, provider or cooker) as
   text; other worker diagnostics are not delivered (R5f).

### `.mesh` blob table failures

A bad blob table fails the asset recoverably (mesh-format-spec §5.9, §7):

| Failure | `Code` | Diagnostic |
|---|---|---|
| Header sizes disagree (`gpuDataSize` vs `GPUD` size vs `fileSize - gpuDataOffset`; with `kPayloadRaw`, vs `payloadDecodedSize`) | `Corrupt` | K4003, K4016 |
| `BLOB` missing, not sorted by `encodedOffset`, or `lodRank` decreasing | `Corrupt` | K4005, K4013 |
| Blob encoded range outside `GPUD`, or decoded range outside `payloadDecodedSize` | `Corrupt` | K4013 |
| Encoded or decoded ranges overlap, or blobs do not cover a range that `LODS` references | `Corrupt` | K4013, K4012 |
| Decoder output size differs from `decodedSize` | `Corrupt` | K4021 |
| `kPayloadRaw` set but a blob is not an identity range | `Corrupt` | K4016 |
| `checksum` mismatch (`DecodeOptions::verifyChecksums`, on in debug builds) | `Corrupt` | K4017 |
| Misaligned offsets, `elementSize` 0, `decodedSize` not a multiple of the codec unit | `ValidationFailed` | K4014 |
| Codec/filter pair not allowed, `kBlobOuterZstd` with a non-`Meshopt*` codec, or a U8 index blob with a `Meshopt*` codec | `ValidationFailed` | K4014 |
| Unknown codec or filter id, or one not built into this runtime (anything but `None` in v0.5) | `Unsupported` | K4015 |

- `Corrupt` means the bytes cannot be trusted (bounds, overlaps, sizes). `ValidationFailed` means
  the file is structurally sound but breaks a spec rule. Both are recoverable.
- Proposed: `Status.detail` carries the blob index so tools can point at the entry. Not
  implemented; `detail` is 0 today.
- The writer validates its input by the same spec rules (`mesh::write`), and `kiln-info --check`
  runs the reader checks on a file.

Never a crash, never a silent failure (v0.5 exit criterion).

### Diagnostic code namespace

- Codes are `u32`, rendered as `K` plus 4 digits (`K1003`). 0 means "no code".
- Codes are stable once released. Each emitting module defines its codes as an enum in its public
  header (`GltfDiagCode`, `ImageDiagCode`, `SettingsDiagCode`, `mesh::DiagCode`, `ktx2::DiagCode`,
  `RuntimeDiagCode`). `../diagnostics.md` documents each with a meaning and an artist-facing fix
  hint. No test checks for duplicate numbers yet.

| Range | Area |
|---|---|
| K1000-1999 | glTF import (unsupported extensions, sparse accessors, Draco, bad node names, missing UVs) |
| K2000-2999 | image import and encode (PNG/JPEG/WebP decode, KTX2 pass-through, size, channel checks) |
| K3000-3999 | settings resolution (invalid combinations, unknown keys later) |
| K4000-4999 | Cooked-file validation: `.mesh` and KTX2 (reader and writer checks, including `BLOB` table checks), the store catalog (K4200-4299) |
| K5000-5999 | runtime and store (store miss, corrupt entry, adapter failures, placeholder failures) |
| K6000-9999 | reserved |

### Exception boundary for third-party code

- kiln code never uses `throw`, `try`, `catch`, `dynamic_cast` or `typeid`.
- If a dependency or std call can throw on a path kiln must report, the **only** `try`/`catch`
  allowed is in the single `.cpp` that calls it. It catches, converts to `Status`, and emits a
  diagnostic where it can. `KILN_HAS_EXCEPTIONS` (`core.h`) compiles the block out when exceptions
  are off.
- The only such block today is the `std::thread` construction in `src/io/thread_pool.cpp` (R5l).
  None of the third-party dependencies throw.

## Rationale

- A 4-byte `Status` is free to return everywhere and keeps `Result<T>` small.
- Separating "what failed" (Status) from "why, in words" (Diagnostic) keeps the hot path
  allocation-free and gives tools structured, documentable output.
- Panicking on misuse keeps API contracts strict without burdening every call with checks the
  caller cannot act on.

## Alternatives considered

`std::expected` (pulls a heavier std header into public headers), a Status with an owned message
string (heap in the error path), a Status with a static `char const*` (loses paths and node names),
and a global error-code registry (a registration step and a global): rejected.

## Consequences / what this constrains later

- `Code` is append-only after the first release. Reordering is an ABI and log-format break.
- Diagnostic numbers are part of the artist-facing documentation; no renumbering after release.
- Any API that can fail recoverably returns `Status` or `Result<T>`; no out-parameter error codes.

## Open points for the owner

- Confirm the `K` + 4-digit code format and the ranges above.
- Confirm "exactly one Error diagnostic per failed load", including that other worker diagnostics
  are not delivered (R5f).
- Confirm that `Status.detail` stays 16 bits (it truncates Windows error codes to their low bits).
- Confirm the `wait()` misuse panics and `pump()` off-thread as a debug assert only.
- Confirm the `Corrupt` vs `ValidationFailed` split for blob-table failures above.
