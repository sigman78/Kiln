// kiln/core.h — config macros, fundamental types, panic/assert, and the basic
// vocabulary types (Span, StrView, Handle, FunctionRef).
#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

// ---------------------------------------------------------------------------
// Compiler / platform detection
// ---------------------------------------------------------------------------

#if defined(_MSC_VER) && !defined(__clang__)
#define KILN_COMPILER_MSVC 1
#elif defined(__clang__)
#define KILN_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define KILN_COMPILER_GCC 1
#endif

#if defined(_WIN32)
#define KILN_OS_WINDOWS 1
#elif defined(__APPLE__)
#define KILN_OS_MACOS 1
#elif defined(__linux__)
#define KILN_OS_LINUX 1
#endif

// ARM64EC also defines _M_X64 but compiles to ARM64 code, so it counts as ARM64.
#if defined(_M_ARM64) || defined(_M_ARM64EC) || defined(__aarch64__)
#define KILN_ARCH_ARM64 1
#elif defined(_M_X64) || defined(__x86_64__)
#define KILN_ARCH_X64 1
#endif

#if defined(KILN_COMPILER_MSVC)
#define KILN_FORCEINLINE __forceinline
#define KILN_NOINLINE __declspec(noinline)
#define KILN_DEBUGBREAK() __debugbreak()
#else
#define KILN_FORCEINLINE inline __attribute__((always_inline))
#define KILN_NOINLINE __attribute__((noinline))
#define KILN_DEBUGBREAK() __builtin_trap()
#endif

// KILN_HOT marks a hot function (optimize harder, place with other hot code); MSVC
// has no equivalent. KILN_RESTRICT promises the pointer is the only access path.
#if defined(KILN_COMPILER_MSVC)
#define KILN_HOT
#else
#define KILN_HOT __attribute__((hot))
#endif
#define KILN_RESTRICT __restrict

// Unreachable code: use std::unreachable() (C++23, <utility>).

// 1 when exceptions are enabled in this translation unit. kiln never throws. It
// catches only around std code that can throw (std::thread), and only when this is 1.
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
#define KILN_HAS_EXCEPTIONS 1
#else
#define KILN_HAS_EXCEPTIONS 0
#endif

// Optimizer hint that `cond` holds. Never evaluates `cond` at runtime; a false
// condition is undefined behavior, so pair hot-path uses with a KILN_ASSERT.
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(assume) >= 202207L
#define KILN_ASSUME(cond) [[assume(cond)]]
#endif
#endif
#if !defined(KILN_ASSUME)
#if defined(KILN_COMPILER_MSVC)
#define KILN_ASSUME(cond) __assume(cond)
#elif defined(KILN_COMPILER_CLANG)
#define KILN_ASSUME(cond) __builtin_assume(cond)
#else
#define KILN_ASSUME(cond)                                                                                    \
    do {                                                                                                     \
        if (!(cond)) __builtin_unreachable();                                                                \
    } while (0)
#endif
#endif

#if defined(KILN_COMPILER_MSVC)
#define KILN_LIKELY(x) (x)
#define KILN_UNLIKELY(x) (x)
#else
#define KILN_LIKELY(x) __builtin_expect(!!(x), 1)
#define KILN_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// printf-style format checking for our own variadic functions. `fmtIdx` is the
// 1-based index of the format parameter, `argIdx` that of the first vararg (0 for
// va_list variants). Member functions count `this` as parameter 1.
#if defined(KILN_COMPILER_MSVC)
#define KILN_PRINTF(fmtIdx, argIdx)
#else
#define KILN_PRINTF(fmtIdx, argIdx) __attribute__((format(printf, fmtIdx, argIdx)))
#endif

// Shared-library export. Empty in static builds (the default). KILN_SHARED and
// KILN_EXPORTS select dllexport/dllimport.
#if defined(KILN_SHARED)
#if defined(_WIN32)
#if defined(KILN_EXPORTS)
#define KILN_API __declspec(dllexport)
#else
#define KILN_API __declspec(dllimport)
#endif
#else
#define KILN_API __attribute__((visibility("default")))
#endif
#else
#define KILN_API
#endif

// KILN_DEBUG: debug-only checks (KILN_ASSERT). Defaults to !NDEBUG.
#if !defined(KILN_DEBUG)
#if defined(NDEBUG)
#define KILN_DEBUG 0
#else
#define KILN_DEBUG 1
#endif
#endif

// ---------------------------------------------------------------------------
// Fundamental types
// ---------------------------------------------------------------------------

namespace kiln {

using u8    = std::uint8_t;
using u16   = std::uint16_t;
using u32   = std::uint32_t;
using u64   = std::uint64_t;
using i8    = std::int8_t;
using i16   = std::int16_t;
using i32   = std::int32_t;
using i64   = std::int64_t;
using f32   = float;
using f64   = double;
using usize = std::size_t;
using isize = std::ptrdiff_t;

static_assert(std::endian::native == std::endian::little, "kiln supports little-endian targets only");

/// Universal "no index" sentinel, matching the .mesh spec.
inline constexpr u32 kInvalid = 0xFFFFFFFFu;

template <class T> [[nodiscard]] constexpr T min(T a, T b) noexcept { return b < a ? b : a; }
template <class T> [[nodiscard]] constexpr T max(T a, T b) noexcept { return a < b ? b : a; }
template <class T> [[nodiscard]] constexpr T clamp(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (hi < v ? hi : v);
}

/// Round `v` up to a multiple of `align` (power of two).
template <std::unsigned_integral T> [[nodiscard]] constexpr T align_up(T v, T align) noexcept {
    return (v + (align - 1)) & ~(align - 1);
}
template <std::unsigned_integral T> [[nodiscard]] constexpr bool is_aligned(T v, T align) noexcept {
    return (v & (align - 1)) == 0;
}
template <std::unsigned_integral T> [[nodiscard]] constexpr bool is_pow2(T v) noexcept {
    return v != 0 && (v & (v - 1)) == 0;
}

// ---------------------------------------------------------------------------
// Panic / assert
// ---------------------------------------------------------------------------

/// Called on non-recoverable errors. Must not return. The default handler logs
/// through the log sink and aborts. Override with set_panic_handler().
using PanicHandler = void (*)(void* user, char const* file, int line, char const* msg);

KILN_API void set_panic_handler(PanicHandler handler, void* user) noexcept;

/// Formats `fmt` with printf-style args and invokes the panic handler. Never returns.
[[noreturn]] KILN_API void panic(char const* file, int line, char const* fmt, ...) noexcept KILN_PRINTF(3, 4);

} // namespace kiln

/// Non-recoverable error: broken invariant, API misuse, allocator OOM. Always on.
#define KILN_PANIC(...) ::kiln::panic(__FILE__, __LINE__, __VA_ARGS__)

/// Always-on check. Use for API contracts and invariants that must hold in release.
#define KILN_VERIFY(cond)                                                                                    \
    do {                                                                                                     \
        if (KILN_UNLIKELY(!(cond))) ::kiln::panic(__FILE__, __LINE__, "verify failed: %s", #cond);           \
    } while (0)

/// Debug-only check. Compiles to nothing when KILN_DEBUG == 0.
#if KILN_DEBUG
#define KILN_ASSERT(cond)                                                                                    \
    do {                                                                                                     \
        if (KILN_UNLIKELY(!(cond))) ::kiln::panic(__FILE__, __LINE__, "assert failed: %s", #cond);           \
    } while (0)
#else
#define KILN_ASSERT(cond)                                                                                    \
    do {                                                                                                     \
    } while (0)
#endif

namespace kiln {

/// Non-owning view over contiguous memory.
template <class T> struct Span {
    T* data    = nullptr;
    usize size = 0;

    constexpr Span() noexcept = default;
    constexpr Span(T* p, usize n) noexcept : data(p), size(n) {}
    template <usize N>
    constexpr Span(T (&arr)[N]) noexcept : data(arr), size(N) {} // NOLINT(google-explicit-constructor)
    /// Span<T const> from Span<T>.
    template <class U>
        requires(std::is_const_v<T> && std::is_same_v<std::remove_const_t<T>, U>)
    constexpr Span(Span<U> other) noexcept : data(other.data), size(other.size) {} // NOLINT

    [[nodiscard]] constexpr bool empty() const noexcept { return size == 0; }
    [[nodiscard]] constexpr usize size_bytes() const noexcept { return size * sizeof(T); }
    [[nodiscard]] constexpr T* begin() const noexcept { return data; }
    [[nodiscard]] constexpr T* end() const noexcept { return data + size; }
    [[nodiscard]] constexpr T& front() const noexcept {
        KILN_ASSERT(size > 0);
        return data[0];
    }
    [[nodiscard]] constexpr T& back() const noexcept {
        KILN_ASSERT(size > 0);
        return data[size - 1];
    }
    [[nodiscard]] constexpr T& operator[](usize i) const noexcept {
        KILN_ASSERT(i < size);
        return data[i];
    }
    [[nodiscard]] constexpr Span<T> first(usize n) const noexcept {
        KILN_ASSERT(n <= size);
        return {data, n};
    }
    [[nodiscard]] constexpr Span<T> last(usize n) const noexcept {
        KILN_ASSERT(n <= size);
        return {data + (size - n), n};
    }
    [[nodiscard]] constexpr Span<T> subspan(usize off, usize n) const noexcept {
        KILN_ASSERT(off <= size && n <= size - off);
        return {data + off, n};
    }
    [[nodiscard]] constexpr Span<T> subspan(usize off) const noexcept {
        KILN_ASSERT(off <= size);
        return {data + off, size - off};
    }
};

template <class T> Span(T*, usize) -> Span<T>;

using ByteSpan      = Span<u8>;
using ConstByteSpan = Span<u8 const>;

template <class T> [[nodiscard]] constexpr Span<u8 const> as_bytes(Span<T> s) noexcept {
    return {reinterpret_cast<u8 const*>(s.data), s.size_bytes()};
}
template <class T>
    requires(!std::is_const_v<T>)
[[nodiscard]] constexpr Span<u8> as_writable_bytes(Span<T> s) noexcept {
    return {reinterpret_cast<u8*>(s.data), s.size_bytes()};
}

/// Non-owning string view. Not necessarily null-terminated.
struct StrView {
    char const* data = nullptr;
    usize size       = 0;

    constexpr StrView() noexcept = default;
    constexpr StrView(char const* p, usize n) noexcept : data(p), size(n) {}
    constexpr StrView(char const* cstr) noexcept // NOLINT(google-explicit-constructor)
        : data(cstr), size(cstr ? cstr_len(cstr) : 0) {}

    [[nodiscard]] constexpr bool empty() const noexcept { return size == 0; }
    [[nodiscard]] constexpr char const* begin() const noexcept { return data; }
    [[nodiscard]] constexpr char const* end() const noexcept { return data + size; }
    [[nodiscard]] constexpr char operator[](usize i) const noexcept {
        KILN_ASSERT(i < size);
        return data[i];
    }
    [[nodiscard]] constexpr char front() const noexcept {
        KILN_ASSERT(size > 0);
        return data[0];
    }
    [[nodiscard]] constexpr char back() const noexcept {
        KILN_ASSERT(size > 0);
        return data[size - 1];
    }
    [[nodiscard]] constexpr StrView substr(usize off, usize n) const noexcept {
        KILN_ASSERT(off <= size);
        return {data + off, min(n, size - off)};
    }
    [[nodiscard]] constexpr StrView substr(usize off) const noexcept {
        KILN_ASSERT(off <= size);
        return {data + off, size - off};
    }
    [[nodiscard]] constexpr bool starts_with(StrView p) const noexcept {
        return size >= p.size && equal_n(data, p.data, p.size);
    }
    [[nodiscard]] constexpr bool ends_with(StrView p) const noexcept {
        return size >= p.size && equal_n(data + (size - p.size), p.data, p.size);
    }
    /// Index of first occurrence of `c` at or after `from`, or kNpos.
    [[nodiscard]] constexpr usize find(char c, usize from = 0) const noexcept {
        for (usize i = from; i < size; ++i)
            if (data[i] == c) return i;
        return kNpos;
    }
    [[nodiscard]] constexpr usize rfind(char c) const noexcept {
        for (usize i = size; i-- > 0;)
            if (data[i] == c) return i;
        return kNpos;
    }
    [[nodiscard]] constexpr Span<char const> bytes() const noexcept { return {data, size}; }

    static constexpr usize kNpos = ~usize(0);

    static constexpr usize cstr_len(char const* s) noexcept {
        usize n = 0;
        while (s[n] != '\0')
            ++n;
        return n;
    }
    static constexpr bool equal_n(char const* a, char const* b, usize n) noexcept {
        for (usize i = 0; i < n; ++i)
            if (a[i] != b[i]) return false;
        return true;
    }
};

[[nodiscard]] constexpr bool operator==(StrView a, StrView b) noexcept {
    return a.size == b.size && StrView::equal_n(a.data, b.data, a.size);
}
[[nodiscard]] constexpr bool operator!=(StrView a, StrView b) noexcept { return !(a == b); }

/// Three-way lexicographic compare (bytewise): <0, 0, >0.
[[nodiscard]] constexpr int compare(StrView a, StrView b) noexcept {
    usize n = min(a.size, b.size);
    for (usize i = 0; i < n; ++i) {
        if (a.data[i] != b.data[i]) return u8(a.data[i]) < u8(b.data[i]) ? -1 : 1;
    }
    return a.size == b.size ? 0 : (a.size < b.size ? -1 : 1);
}

inline namespace literals {
constexpr StrView operator""_sv(char const* s, usize n) noexcept { return {s, n}; }
} // namespace literals

/// Typed, generation-counted index. `Tag` is a phantom type (e.g. `struct Mesh;`).
/// Generation 0 is never issued, so it means null. The default handle is null and
/// equals `from_bits(0)`. See docs/design/handles-and-states.md.
template <class Tag> struct Handle {
    u32 index      = 0;
    u32 generation = 0;

    [[nodiscard]] constexpr bool is_null() const noexcept { return generation == 0; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return !is_null(); }
    [[nodiscard]] constexpr u64 bits() const noexcept { return (u64(generation) << 32) | index; }
    [[nodiscard]] static constexpr Handle from_bits(u64 b) noexcept { return {u32(b), u32(b >> 32)}; }

    [[nodiscard]] friend constexpr bool operator==(Handle a, Handle b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
    [[nodiscard]] friend constexpr bool operator!=(Handle a, Handle b) noexcept { return !(a == b); }
};

/// Non-owning reference to any callable.
template <class Sig> class FunctionRef;

template <class R, class... Args> class FunctionRef<R(Args...)> {
public:
    constexpr FunctionRef() noexcept = default;
    constexpr FunctionRef(std::nullptr_t) noexcept {} // NOLINT(google-explicit-constructor)

    /// From a plain function pointer.
    constexpr FunctionRef(R (*fn)(Args...)) noexcept // NOLINT(google-explicit-constructor)
        : obj_(reinterpret_cast<void*>(fn)), thunk_(&fn_thunk) {}

    /// From a C-style `fn(user, args...)` pair.
    constexpr FunctionRef(R (*fn)(void*, Args...), void* user) noexcept : obj_(user), thunk_(fn) {}

    /// From any callable object. The object must outlive the FunctionRef.
    template <class F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, FunctionRef> &&
                 !std::is_function_v<std::remove_pointer_t<std::remove_reference_t<F>>> &&
                 std::is_invocable_r_v<R, F&, Args...>)
    constexpr FunctionRef(F&& f) noexcept // NOLINT(google-explicit-constructor)
        : obj_(const_cast<void*>(static_cast<void const*>(&f))),
          thunk_(&obj_thunk<std::remove_reference_t<F>>) {}

    constexpr R operator()(Args... args) const {
        KILN_ASSERT(thunk_ != nullptr);
        return thunk_(obj_, std::forward<Args>(args)...);
    }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return thunk_ != nullptr; }

private:
    static R fn_thunk(void* obj, Args... args) {
        return reinterpret_cast<R (*)(Args...)>(obj)(std::forward<Args>(args)...);
    }
    template <class F> static R obj_thunk(void* obj, Args... args) {
        return (*static_cast<F*>(obj))(std::forward<Args>(args)...);
    }

    void* obj_                  = nullptr;
    R (*thunk_)(void*, Args...) = nullptr;
};

template <class T, usize N> [[nodiscard]] constexpr usize countof(T const (&)[N]) noexcept { return N; }

/// Read or write a trivially copyable T at a possibly unaligned address.
template <class T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] inline T read_unaligned(void const* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}
template <class T>
    requires std::is_trivially_copyable_v<T>
inline void write_unaligned(void* p, T const& v) noexcept {
    std::memcpy(p, &v, sizeof(T));
}

} // namespace kiln
