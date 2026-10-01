// kiln/result.h — Status, Result<T> and the diagnostic sink. No heap in the error path.
// See docs/design/error-model.md.
#pragma once

#include "kiln/core.h"

namespace kiln {

/// Error codes. Stable across versions once released; append only.
enum class Code : u16 {
    Ok = 0,
    Unknown,         ///< unclassified failure
    InvalidArgument, ///< caller passed something the API rejects
    OutOfMemory,     ///< allocation failed where the caller opted in to recover (rare)
    NotFound,        ///< asset, file, key or entry does not exist
    AlreadyExists,
    Unsupported,      ///< format, extension or feature not supported (by design or yet)
    IoError,          ///< OS-level IO failure; `detail` carries errno / GetLastError() low bits
    IoEof,            ///< read past end of file
    ParseError,       ///< malformed input (glTF, PNG, KTX2, config)
    ValidationFailed, ///< well-formed input violating a semantic rule
    Corrupt,          ///< cooked data failed integrity checks
    VersionMismatch,  ///< cooked data from an incompatible format/cooker version
    Busy,             ///< back-pressure: try again later (adapter, budgets)
    NotReady,         ///< requested thing is not in a state that allows the operation
    Cancelled,
    Timeout,
    Internal, ///< invariant that should have panicked but was caught at a boundary
    Count
};

KILN_API char const* code_name(Code c);

/// A compact status: error code plus a 16-bit code-specific detail (e.g. errno).
struct [[nodiscard]] Status {
    Code code  = Code::Ok;
    u16 detail = 0;

    [[nodiscard]] constexpr bool ok() const noexcept { return code == Code::Ok; }
    [[nodiscard]] constexpr bool failed() const noexcept { return code != Code::Ok; }

    [[nodiscard]] friend constexpr bool operator==(Status a, Status b) noexcept {
        return a.code == b.code && a.detail == b.detail;
    }
    [[nodiscard]] friend constexpr bool operator!=(Status a, Status b) noexcept { return !(a == b); }
};

inline constexpr Status kOk{};

constexpr Status make_status(Code c, u16 detail = 0) { return {c, detail}; }

/// Value-or-Status. Construct from a T (success) or a Status/Code (failure).
/// Accessing the value of a failed Result is a programming error (asserts).
template <class T> class [[nodiscard]] Result {
public:
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported; use Result<T*>");
    static_assert(NothrowStorable<T>, "Result<T> needs T whose copy, move and destruction never throw");

    constexpr Result(Status s) noexcept : status_(s) { // NOLINT(google-explicit-constructor)
        KILN_ASSERT(s.failed() && "Result constructed from Ok status without a value");
    }
    constexpr Result(Code c) noexcept : Result(make_status(c)) {} // NOLINT(google-explicit-constructor)

    // Not constexpr: placement new is not constant-evaluable before C++26.
    Result(T const& v) noexcept(std::is_nothrow_copy_constructible_v<T>) // NOLINT
        requires std::is_copy_constructible_v<T>
    {
        ::new (static_cast<void*>(storage_)) T(v);
        status_ = kOk;
    }
    Result(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) // NOLINT
    {
        ::new (static_cast<void*>(storage_)) T(std::move(v));
        status_ = kOk;
    }

    Result(Result const& o) noexcept(std::is_nothrow_copy_constructible_v<T>)
        requires std::is_copy_constructible_v<T>
        : status_(o.status_) {
        if (o.ok()) ::new (static_cast<void*>(storage_)) T(o.ref());
    }
    Result(Result&& o) noexcept(std::is_nothrow_move_constructible_v<T>) : status_(o.status_) {
        if (o.ok()) ::new (static_cast<void*>(storage_)) T(std::move(o.ref()));
    }
    Result& operator=(Result const& o) noexcept(std::is_nothrow_copy_constructible_v<T>)
        requires std::is_copy_constructible_v<T>
    {
        if (this != &o) {
            destroy();
            status_ = o.status_;
            if (o.ok()) ::new (static_cast<void*>(storage_)) T(o.ref());
        }
        return *this;
    }
    Result& operator=(Result&& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &o) {
            destroy();
            status_ = o.status_;
            if (o.ok()) ::new (static_cast<void*>(storage_)) T(std::move(o.ref()));
        }
        return *this;
    }
    ~Result() noexcept { destroy(); }

    [[nodiscard]] constexpr bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] constexpr bool failed() const noexcept { return status_.failed(); }
    constexpr Status status() const noexcept { return status_; }
    constexpr Code code() const noexcept { return status_.code; }

    T& value() & noexcept {
        KILN_ASSERT(ok());
        return ref();
    }
    T const& value() const& noexcept {
        KILN_ASSERT(ok());
        return ref();
    }
    T&& value() && noexcept {
        KILN_ASSERT(ok());
        return std::move(ref());
    }
    T& operator*() & noexcept { return value(); }
    T const& operator*() const& noexcept { return value(); }
    T&& operator*() && noexcept { return std::move(*this).value(); }
    T* operator->() noexcept { return &value(); }
    T const* operator->() const noexcept { return &value(); }

    T value_or(T fallback) const& { return ok() ? ref() : std::move(fallback); }
    T value_or(T fallback) && { return ok() ? std::move(ref()) : std::move(fallback); }

    // Monadic composition, as in std::expected. No heap.

    /// f(T) -> Result<U>. On failure the Status is forwarded unchanged.
    template <class F> auto and_then(F&& f) && -> std::invoke_result_t<F, T&&> {
        using R = std::invoke_result_t<F, T&&>;
        return ok() ? f(std::move(ref())) : R(status_);
    }
    template <class F> auto and_then(F&& f) const& -> std::invoke_result_t<F, T const&> {
        using R = std::invoke_result_t<F, T const&>;
        return ok() ? f(ref()) : R(status_);
    }
    /// f(T) -> U, wrapped into Result<U>. On failure the Status is forwarded unchanged.
    template <class F> auto transform(F&& f) && -> Result<std::invoke_result_t<F, T&&>> {
        using R = Result<std::invoke_result_t<F, T&&>>;
        return ok() ? R(f(std::move(ref()))) : R(status_);
    }
    template <class F> auto transform(F&& f) const& -> Result<std::invoke_result_t<F, T const&>> {
        using R = Result<std::invoke_result_t<F, T const&>>;
        return ok() ? R(f(ref())) : R(status_);
    }
    /// f(Status) -> Result<T>, called only on failure (recovery / substitution).
    template <class F> Result or_else(F&& f) && { return ok() ? std::move(*this) : Result(f(status_)); }

private:
    T& ref() noexcept { return *std::launder(reinterpret_cast<T*>(storage_)); }
    T const& ref() const noexcept { return *std::launder(reinterpret_cast<T const*>(storage_)); }
    void destroy() noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            if (ok()) ref().~T();
        }
    }

    alignas(T) unsigned char storage_[sizeof(T)];
    Status status_;
};

/// Result<void> carries only a Status.
template <> class Result<void> {
public:
    constexpr Result() noexcept = default;
    constexpr Result(Status s) noexcept : status_(s) {}            // NOLINT(google-explicit-constructor)
    constexpr Result(Code c) noexcept : status_(make_status(c)) {} // NOLINT(google-explicit-constructor)

    [[nodiscard]] constexpr bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] constexpr bool failed() const noexcept { return status_.failed(); }
    constexpr Status status() const noexcept { return status_; }
    constexpr Code code() const noexcept { return status_.code; }

    /// f() -> Result<U>; the Status is forwarded on failure.
    template <class F> auto and_then(F&& f) const -> std::invoke_result_t<F> {
        using R = std::invoke_result_t<F>;
        return ok() ? f() : R(status_);
    }
    /// f() -> U, wrapped into Result<U>.
    template <class F> auto transform(F&& f) const -> Result<std::invoke_result_t<F>> {
        using R = Result<std::invoke_result_t<F>>;
        return ok() ? R(f()) : R(status_);
    }

private:
    Status status_{};
};

/// Early-return helper: evaluates `expr` (a Status or Result) and returns its Status
/// from the enclosing function if it failed.
#define KILN_TRY(expr)                                                                                       \
    do {                                                                                                     \
        auto&& kiln_try_tmp_ = (expr);                                                                       \
        if (KILN_UNLIKELY(kiln_try_tmp_.failed())) return ::kiln::detail::to_status(kiln_try_tmp_);          \
    } while (0)

/// Early-return helper that binds the value on success: KILN_TRY_ASSIGN(auto v, f());
#define KILN_TRY_ASSIGN(decl, expr)                                                                          \
    auto&& KILN_CAT_(kiln_try_r_, __LINE__) = (expr);                                                        \
    if (KILN_UNLIKELY(KILN_CAT_(kiln_try_r_, __LINE__).failed()))                                            \
        return ::kiln::detail::to_status(KILN_CAT_(kiln_try_r_, __LINE__));                                  \
    decl = std::move(KILN_CAT_(kiln_try_r_, __LINE__)).value()

#define KILN_CAT2_(a, b) a##b
#define KILN_CAT_(a, b) KILN_CAT2_(a, b)

namespace detail {
constexpr Status to_status(Status s) { return s; }
template <class T> constexpr Status to_status(Result<T> const& r) { return r.status(); }
} // namespace detail

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

enum class Severity : u8 { Info = 0, Warning, Error };

KILN_API char const* severity_name(Severity s);

/// A structured diagnostic. `code` is a stable, documentable identifier defined by
/// the emitting module (e.g. cook diagnostics use a "Kxxxx" table); 0 = none.
/// All views are valid only for the duration of the sink callback.
struct Diagnostic {
    u32 code          = 0;
    Severity severity = Severity::Error;
    Status status     = kOk; ///< the Status this diagnostic accompanies, if any
    StrView asset;           ///< asset path / id text, may be empty
    StrView where;           ///< node, material, section... may be empty
    StrView message;
};

/// Caller-provided sink. A null sink (or null fn) drops diagnostics.
struct DiagSink {
    void (*fn)(void* user, Diagnostic const& d) = nullptr;
    void* user                                  = nullptr;
};

inline void emit(DiagSink const* sink, Diagnostic const& d) {
    if (sink && sink->fn) sink->fn(sink->user, d);
}

/// Format a message (printf-style, stack buffer) and emit it. Returns `status` for
/// `return diagf(sink, status, ...)` chains.
KILN_API Status diagf(DiagSink const* sink, Status status, u32 code, Severity severity, StrView asset,
                      StrView where, char const* fmt, ...) KILN_PRINTF(7, 8);

/// A DiagSink that forwards to the log (category "diag").
KILN_API DiagSink log_diag_sink();

} // namespace kiln
