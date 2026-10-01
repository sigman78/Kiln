// kiln/containers.h — FixedArray<T,N>, Vec<T>, HashMap<K,V>.
// Vec and HashMap allocate through their Allocator and Tag, only on growth.
// A null Allocator means default_allocator(). Index checks are debug-only.
#pragma once

#include "kiln/alloc.h"
#include "kiln/hash.h"

namespace kiln {

namespace detail {

/// Relocate `n` objects from `src` to uninitialized `dst`, destroying the source.
template <class T> inline void relocate_n(T* dst, T* src, usize n) noexcept {
    if constexpr (std::is_trivially_copyable_v<T>) {
        if (n) std::memcpy(static_cast<void*>(dst), static_cast<void const*>(src), n * sizeof(T));
    } else {
        for (usize i = 0; i < n; ++i) {
            ::new (static_cast<void*>(dst + i)) T(std::move(src[i]));
            src[i].~T();
        }
    }
}
template <class T> inline void destroy_n(T* p, usize n) noexcept {
    if constexpr (!std::is_trivially_destructible_v<T>) {
        for (usize i = 0; i < n; ++i)
            p[i].~T();
    } else {
        (void)p;
        (void)n;
    }
}
template <class T> inline void default_construct_n(T* p, usize n) noexcept {
    if constexpr (std::is_trivially_default_constructible_v<T>) {
        if (n) std::memset(static_cast<void*>(p), 0, n * sizeof(T));
    } else {
        for (usize i = 0; i < n; ++i)
            ::new (static_cast<void*>(p + i)) T();
    }
}

} // namespace detail

/// Inline storage for up to N elements. Never allocates.
template <class T, usize N> class FixedArray {
public:
    static_assert(N > 0);
    static_assert(NothrowStorable<T>, "FixedArray<T> needs T whose copy, move and destruction never throw");

    constexpr FixedArray() noexcept = default;
    ~FixedArray() noexcept { clear(); }

    FixedArray(FixedArray const& o) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        for (usize i = 0; i < o.size_; ++i)
            ::new (slot(i)) T(o[i]);
        size_ = o.size_;
    }
    FixedArray(FixedArray&& o) noexcept {
        detail::relocate_n(ptr(), o.ptr(), o.size_);
        size_   = o.size_;
        o.size_ = 0;
    }
    FixedArray& operator=(FixedArray const& o) noexcept(std::is_nothrow_copy_constructible_v<T>) {
        if (this != &o) {
            clear();
            for (usize i = 0; i < o.size_; ++i)
                ::new (slot(i)) T(o[i]);
            size_ = o.size_;
        }
        return *this;
    }
    FixedArray& operator=(FixedArray&& o) noexcept {
        if (this != &o) {
            clear();
            detail::relocate_n(ptr(), o.ptr(), o.size_);
            size_   = o.size_;
            o.size_ = 0;
        }
        return *this;
    }

    constexpr usize size() const noexcept { return size_; }
    static constexpr usize capacity() noexcept { return N; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ == N; }

    T* data() noexcept { return ptr(); }
    T const* data() const noexcept { return ptr(); }
    T* begin() noexcept { return ptr(); }
    T* end() noexcept { return ptr() + size_; }
    T const* begin() const noexcept { return ptr(); }
    T const* end() const noexcept { return ptr() + size_; }

    T& operator[](usize i) noexcept {
        KILN_ASSERT(i < size_);
        return ptr()[i];
    }
    T const& operator[](usize i) const noexcept {
        KILN_ASSERT(i < size_);
        return ptr()[i];
    }
    T& front() noexcept {
        KILN_ASSERT(size_ > 0);
        return ptr()[0];
    }
    T& back() noexcept {
        KILN_ASSERT(size_ > 0);
        return ptr()[size_ - 1];
    }
    T const& front() const noexcept {
        KILN_ASSERT(size_ > 0);
        return ptr()[0];
    }
    T const& back() const noexcept {
        KILN_ASSERT(size_ > 0);
        return ptr()[size_ - 1];
    }

    Span<T> span() noexcept { return {ptr(), size_}; }
    Span<T const> span() const noexcept { return {ptr(), size_}; }

    /// Append. Panics when full.
    T& push_back(T const& v) {
        KILN_VERIFY(size_ < N);
        return *::new (slot(size_++)) T(v);
    }
    T& push_back(T&& v) {
        KILN_VERIFY(size_ < N);
        return *::new (slot(size_++)) T(std::move(v));
    }
    template <class... Args> T& emplace_back(Args&&... args) {
        KILN_VERIFY(size_ < N);
        return *::new (slot(size_++)) T(std::forward<Args>(args)...);
    }
    /// Append if room; returns false when full.
    [[nodiscard]] bool try_push_back(T const& v) {
        if (size_ == N) return false;
        ::new (slot(size_++)) T(v);
        return true;
    }
    void pop_back() noexcept {
        KILN_ASSERT(size_ > 0);
        ptr()[--size_].~T();
    }
    /// Remove by swapping the last element into `i`. O(1), does not preserve order.
    void erase_unordered(usize i) noexcept {
        KILN_ASSERT(i < size_);
        if (i != size_ - 1) ptr()[i] = std::move(ptr()[size_ - 1]);
        pop_back();
    }
    void resize(usize n) noexcept {
        KILN_VERIFY(n <= N);
        if (n < size_)
            detail::destroy_n(ptr() + n, size_ - n);
        else
            detail::default_construct_n(ptr() + size_, n - size_);
        size_ = n;
    }
    void clear() noexcept {
        detail::destroy_n(ptr(), size_);
        size_ = 0;
    }

private:
    T* ptr() noexcept { return std::launder(reinterpret_cast<T*>(storage_)); }
    T const* ptr() const noexcept { return std::launder(reinterpret_cast<T const*>(storage_)); }
    void* slot(usize i) noexcept { return static_cast<void*>(storage_ + i * sizeof(T)); }

    alignas(T) unsigned char storage_[N * sizeof(T)];
    usize size_ = 0;
};

/// Growable array over an Allocator.
template <class T> class Vec {
public:
    static_assert(NothrowStorable<T>, "Vec<T> needs T whose copy, move and destruction never throw");
    Vec() noexcept = default;
    explicit Vec(Allocator const* alloc, Tag tag = Tag::Core) noexcept : alloc_(alloc), tag_(tag) {}
    ~Vec() noexcept { release(); }

    Vec(Vec const&)            = delete; ///< Use clone(): copies allocate, so they are explicit.
    Vec& operator=(Vec const&) = delete;

    Vec(Vec&& o) noexcept : data_(o.data_), size_(o.size_), cap_(o.cap_), alloc_(o.alloc_), tag_(o.tag_) {
        o.data_ = nullptr;
        o.size_ = o.cap_ = 0;
    }
    Vec& operator=(Vec&& o) noexcept {
        if (this != &o) {
            release();
            data_   = o.data_;
            size_   = o.size_;
            cap_    = o.cap_;
            alloc_  = o.alloc_;
            tag_    = o.tag_;
            o.data_ = nullptr;
            o.size_ = o.cap_ = 0;
        }
        return *this;
    }

    /// Set the allocator. Call before the first allocation.
    void init(Allocator const* alloc, Tag tag = Tag::Core) noexcept {
        KILN_ASSERT(data_ == nullptr && "Vec::init after allocation");
        alloc_ = alloc;
        tag_   = tag;
    }

    [[nodiscard]] Vec clone() const {
        Vec c(alloc_, tag_);
        c.reserve(size_);
        for (usize i = 0; i < size_; ++i)
            ::new (static_cast<void*>(c.data_ + i)) T(data_[i]);
        c.size_ = size_;
        return c;
    }

    usize size() const noexcept { return size_; }
    usize capacity() const noexcept { return cap_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    Allocator const* allocator() const noexcept { return alloc_; }
    Tag tag() const noexcept { return tag_; }

    T* data() noexcept { return data_; }
    T const* data() const noexcept { return data_; }
    T* begin() noexcept { return data_; }
    T* end() noexcept { return data_ + size_; }
    T const* begin() const noexcept { return data_; }
    T const* end() const noexcept { return data_ + size_; }

    T& operator[](usize i) noexcept {
        KILN_ASSERT(i < size_);
        return data_[i];
    }
    T const& operator[](usize i) const noexcept {
        KILN_ASSERT(i < size_);
        return data_[i];
    }
    T& front() noexcept {
        KILN_ASSERT(size_ > 0);
        return data_[0];
    }
    T& back() noexcept {
        KILN_ASSERT(size_ > 0);
        return data_[size_ - 1];
    }
    T const& front() const noexcept {
        KILN_ASSERT(size_ > 0);
        return data_[0];
    }
    T const& back() const noexcept {
        KILN_ASSERT(size_ > 0);
        return data_[size_ - 1];
    }

    Span<T> span() noexcept { return {data_, size_}; }
    Span<T const> span() const noexcept { return {data_, size_}; }

    void reserve(usize n) {
        if (n > cap_) grow_to(n);
    }
    void resize(usize n) {
        if (n > cap_) grow_to(n);
        if (n < size_)
            detail::destroy_n(data_ + n, size_ - n);
        else
            detail::default_construct_n(data_ + size_, n - size_);
        size_ = n;
    }
    /// `fill` may be an element of this Vec.
    void resize(usize n, T const& fill) {
        auto const fill_from = [&](T* d) noexcept {
            for (usize i = size_; i < n; ++i)
                ::new (static_cast<void*>(d + i)) T(fill);
        };
        if (n > cap_)
            grow_with(n, fill_from);
        else if (n < size_)
            detail::destroy_n(data_ + n, size_ - n);
        else
            fill_from(data_);
        size_ = n;
    }
    /// Grow size by `n` uninitialized elements and return a span of them.
    /// The caller writes them. Use only for trivial T.
    Span<T> append_uninit(usize n) {
        usize old = size_;
        if (n > cap_ - size_) grow_to(grown(n));
        size_ += n;
        return {data_ + old, n};
    }

    // The arguments of push_back, emplace_back and append may refer into this Vec: when it grows,
    // the new elements are built before the old buffer is freed.
    T& push_back(T const& v) { return emplace_back(v); }
    T& push_back(T&& v) { return emplace_back(std::move(v)); }
    template <class... Args> T& emplace_back(Args&&... args) {
        auto const make = [&](T* d) noexcept {
            ::new (static_cast<void*>(d + size_)) T(std::forward<Args>(args)...);
        };
        if (size_ == cap_)
            grow_with(grown(1), make);
        else
            make(data_);
        return data_[size_++];
    }
    void append(Span<T const> items) {
        auto const copy = [&](T* d) noexcept {
            for (usize i = 0; i < items.size; ++i)
                ::new (static_cast<void*>(d + size_ + i)) T(items[i]);
        };
        if (items.size > cap_ - size_)
            grow_with(grown(items.size), copy);
        else
            copy(data_);
        size_ += items.size;
    }
    void pop_back() noexcept {
        KILN_ASSERT(size_ > 0);
        data_[--size_].~T();
    }
    /// Swap-remove: O(1), does not preserve order.
    void erase_unordered(usize i) noexcept {
        KILN_ASSERT(i < size_);
        if (i != size_ - 1) data_[i] = std::move(data_[size_ - 1]);
        pop_back();
    }
    /// Ordered remove: O(n).
    void erase(usize i) noexcept {
        KILN_ASSERT(i < size_);
        for (usize j = i + 1; j < size_; ++j)
            data_[j - 1] = std::move(data_[j]);
        pop_back();
    }
    /// Destroy all elements, keep capacity.
    void clear() noexcept {
        detail::destroy_n(data_, size_);
        size_ = 0;
    }
    /// Destroy all elements and free memory.
    void release() noexcept {
        if (data_) {
            detail::destroy_n(data_, size_);
            free_array(alloc_, data_, cap_, tag_);
        }
        data_ = nullptr;
        size_ = cap_ = 0;
    }

private:
    /// size_ + n, panicking on overflow.
    usize grown(usize n) const noexcept {
        usize total = 0;
        KILN_VERIFY(checked_add(size_, n, total) && "Vec: size overflows");
        return total;
    }
    void grow_to(usize minCap) {
        grow_with(minCap, [](T*) noexcept {});
    }
    /// Moves to a buffer of at least `minCap`. `build(newData)` constructs the new elements first,
    /// while the old buffer, which its arguments may point into, is still alive.
    template <class F> void grow_with(usize minCap, F&& build) {
        KILN_ASSERT(minCap > cap_);
        if (!alloc_) alloc_ = default_allocator();
        usize newCap  = minCap < 4 ? 4 : minCap;
        usize doubled = 0; // amortize: at least double, unless that overflows
        if (checked_mul(cap_, usize(2), doubled) && newCap < doubled) newCap = doubled;
        T* nd = alloc_array<T>(alloc_, newCap, tag_);
        build(nd);
        detail::relocate_n(nd, data_, size_);
        if (data_) free_array(alloc_, data_, cap_, tag_);
        data_ = nd;
        cap_  = newCap;
    }

    T* data_                = nullptr;
    usize size_             = 0;
    usize cap_              = 0;
    Allocator const* alloc_ = nullptr;
    Tag tag_                = Tag::Core;
};

/// Open-addressing hash map with linear probing. K is equality-comparable, and
/// `hash_of(K)` is found by ADL unless a custom `Hash` is given. K and V are
/// move-constructible. Insertion and erasure invalidate pointers into the map.
template <class K, class V, class Hash = DefaultHash> class HashMap {
public:
    static_assert(NothrowStorable<K> && NothrowStorable<V>,
                  "HashMap<K, V> needs K and V whose copy, move and destruction never throw");
    struct Entry {
        K key;
        V value;
    };
    struct InsertResult {
        V* value;      ///< the stored value (new or existing)
        bool inserted; ///< false if the key already existed
    };

    HashMap() noexcept = default;
    explicit HashMap(Allocator const* alloc, Tag tag = Tag::Core) noexcept : alloc_(alloc), tag_(tag) {}
    ~HashMap() noexcept { release(); }

    HashMap(HashMap const&)            = delete;
    HashMap& operator=(HashMap const&) = delete;

    HashMap(HashMap&& o) noexcept
        : hashes_(o.hashes_), entries_(o.entries_), size_(o.size_), cap_(o.cap_), alloc_(o.alloc_),
          tag_(o.tag_) {
        o.hashes_  = nullptr;
        o.entries_ = nullptr;
        o.size_ = o.cap_ = 0;
    }
    HashMap& operator=(HashMap&& o) noexcept {
        if (this != &o) {
            release();
            hashes_    = o.hashes_;
            entries_   = o.entries_;
            size_      = o.size_;
            cap_       = o.cap_;
            alloc_     = o.alloc_;
            tag_       = o.tag_;
            o.hashes_  = nullptr;
            o.entries_ = nullptr;
            o.size_ = o.cap_ = 0;
        }
        return *this;
    }

    void init(Allocator const* alloc, Tag tag = Tag::Core) noexcept {
        KILN_ASSERT(hashes_ == nullptr && "HashMap::init after allocation");
        alloc_ = alloc;
        tag_   = tag;
    }

    usize size() const noexcept { return size_; }
    usize capacity() const noexcept { return cap_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    /// Ensure room for `n` entries without rehashing.
    void reserve(usize n) {
        usize need = cap_for(n);
        if (need > cap_) rehash(need);
    }

    V* find(K const& key) noexcept {
        Entry* e = find_entry(key);
        return e ? &e->value : nullptr;
    }
    V const* find(K const& key) const noexcept {
        Entry const* e = find_entry(key);
        return e ? &e->value : nullptr;
    }
    [[nodiscard]] bool contains(K const& key) const noexcept { return find_index(key) != kNpos; }

    /// Insert or overwrite. Returns the stored value.
    V& insert(K const& key, V value) {
        InsertResult r = try_emplace(key, std::move(value));
        if (!r.inserted) *r.value = std::move(value);
        return *r.value;
    }

    /// Insert if absent, constructing V from `args`. Never overwrites. `key` and `args` may refer into
    /// this map.
    template <class... Args> InsertResult try_emplace(K const& key, Args&&... args) {
        if ((size_ + 1) * 8 > cap_ * 7) { // load factor 7/8
            if (V* found = find(key)) return {found, false};
            // Copies taken before the entries move.
            K k(key);
            V v(std::forward<Args>(args)...);
            rehash(cap_for(size_ + 1));
            return place(k, std::move(v));
        }
        return place(key, std::forward<Args>(args)...);
    }

private:
    /// Probes for `key`; constructs the entry if absent. The table has room.
    template <class... Args> InsertResult place(K const& key, Args&&... args) {
        u64 h   = hash_key(key);
        usize m = cap_ - 1;
        usize i = usize(h) & m;
        for (;;) {
            if (hashes_[i] == 0) {
                hashes_[i] = h;
                ::new (static_cast<void*>(&entries_[i].key)) K(key);
                ::new (static_cast<void*>(&entries_[i].value)) V(std::forward<Args>(args)...);
                ++size_;
                return {&entries_[i].value, true};
            }
            if (hashes_[i] == h && entries_[i].key == key) return {&entries_[i].value, false};
            i = (i + 1) & m;
        }
    }

public:
    /// Remove `key`. Returns true if it existed.
    bool erase(K const& key) noexcept {
        usize i = find_index(key);
        if (i == kNpos) return false;
        erase_at(i);
        return true;
    }

    void clear() noexcept {
        if (!cap_) return;
        for (usize i = 0; i < cap_; ++i) {
            if (hashes_[i]) {
                entries_[i].key.~K();
                entries_[i].value.~V();
                hashes_[i] = 0;
            }
        }
        size_ = 0;
    }
    void release() noexcept {
        if (cap_) {
            clear();
            free(alloc_, hashes_, alloc_bytes(cap_), kDefaultAlign, tag_);
        }
        hashes_  = nullptr;
        entries_ = nullptr;
        size_ = cap_ = 0;
    }

    // Iteration: `for (auto& e : map)` yields Entry {key, value} in unspecified order.
    template <bool Const> class Iter {
        using MapPtr = std::conditional_t<Const, HashMap const*, HashMap*>;
        using Ref    = std::conditional_t<Const, Entry const&, Entry&>;
        using Ptr    = std::conditional_t<Const, Entry const*, Entry*>;

    public:
        Iter(MapPtr m, usize i) noexcept : m_(m), i_(i) { skip(); }
        Ref operator*() const noexcept { return m_->entries_[i_]; }
        Ptr operator->() const noexcept { return &m_->entries_[i_]; }
        Iter& operator++() noexcept {
            ++i_;
            skip();
            return *this;
        }
        bool operator==(Iter const& o) const noexcept { return i_ == o.i_; }
        bool operator!=(Iter const& o) const noexcept { return i_ != o.i_; }

    private:
        void skip() noexcept {
            while (i_ < m_->cap_ && m_->hashes_[i_] == 0)
                ++i_;
        }
        MapPtr m_;
        usize i_;
    };
    using iterator       = Iter<false>;
    using const_iterator = Iter<true>;

    iterator begin() noexcept { return {this, 0}; }
    iterator end() noexcept { return {this, cap_}; }
    const_iterator begin() const noexcept { return {this, 0}; }
    const_iterator end() const noexcept { return {this, cap_}; }

private:
    static constexpr usize kNpos   = ~usize(0);
    static constexpr usize kMinCap = 8;

    static u64 hash_key(K const& key) noexcept {
        u64 h = Hash{}(key);
        return h == 0 ? 1 : h; // 0 is the empty marker
    }
    static constexpr usize cap_for(usize n) noexcept {
        // smallest power of two with n <= cap * 7/8
        usize cap = kMinCap;
        while (n * 8 > cap * 7)
            cap *= 2;
        return cap;
    }
    static usize alloc_bytes(usize cap) noexcept {
        usize hashes = 0, entries = 0, total = 0;
        KILN_VERIFY(checked_mul(cap, sizeof(u64), hashes) && checked_mul(cap, sizeof(Entry), entries) &&
                    checked_add(align_up(hashes, alignof(Entry)), entries, total) &&
                    "HashMap: size overflows");
        return total;
    }

    /// Slot pointer for `key`, or nullptr. Kept separate from find_index so the
    /// address is only formed on the found path (keeps gcc's -Wnull-dereference quiet).
    Entry* find_entry(K const& key) const noexcept {
        if (!cap_) return nullptr;
        u64 h   = hash_key(key);
        usize m = cap_ - 1;
        usize i = usize(h) & m;
        for (;;) {
            if (hashes_[i] == 0) return nullptr;
            if (hashes_[i] == h && entries_[i].key == key) return entries_ + i;
            i = (i + 1) & m;
        }
    }

    usize find_index(K const& key) const noexcept {
        if (!cap_) return kNpos;
        u64 h   = hash_key(key);
        usize m = cap_ - 1;
        usize i = usize(h) & m;
        for (;;) {
            if (hashes_[i] == 0) return kNpos;
            if (hashes_[i] == h && entries_[i].key == key) return i;
            i = (i + 1) & m;
        }
    }

    void erase_at(usize i) noexcept {
        usize m = cap_ - 1;
        entries_[i].key.~K();
        entries_[i].value.~V();
        hashes_[i] = 0;
        --size_;
        // backward-shift: pull later entries of the same probe run into the hole
        usize j = (i + 1) & m;
        while (hashes_[j] != 0) {
            usize ideal = usize(hashes_[j]) & m;
            if (((j - ideal) & m) >= ((j - i) & m)) {
                hashes_[i] = hashes_[j];
                ::new (static_cast<void*>(&entries_[i].key)) K(std::move(entries_[j].key));
                ::new (static_cast<void*>(&entries_[i].value)) V(std::move(entries_[j].value));
                entries_[j].key.~K();
                entries_[j].value.~V();
                hashes_[j] = 0;
                i          = j;
            }
            j = (j + 1) & m;
        }
    }

    void rehash(usize newCap) {
        KILN_ASSERT(is_pow2(newCap));
        if (!alloc_) alloc_ = default_allocator();
        u64* oldHashes    = hashes_;
        Entry* oldEntries = entries_;
        usize oldCap      = cap_;

        u8* mem  = static_cast<u8*>(alloc(alloc_, alloc_bytes(newCap), kDefaultAlign, tag_));
        hashes_  = reinterpret_cast<u64*>(mem);
        entries_ = reinterpret_cast<Entry*>(mem + align_up(newCap * sizeof(u64), alignof(Entry)));
        cap_     = newCap;
        std::memset(static_cast<void*>(hashes_), 0, newCap * sizeof(u64));

        usize m = newCap - 1;
        for (usize s = 0; s < oldCap; ++s) {
            if (oldHashes[s] == 0) continue;
            usize i = usize(oldHashes[s]) & m;
            while (hashes_[i] != 0)
                i = (i + 1) & m;
            hashes_[i] = oldHashes[s];
            ::new (static_cast<void*>(&entries_[i].key)) K(std::move(oldEntries[s].key));
            ::new (static_cast<void*>(&entries_[i].value)) V(std::move(oldEntries[s].value));
            oldEntries[s].key.~K();
            oldEntries[s].value.~V();
        }
        if (oldHashes) free(alloc_, oldHashes, alloc_bytes(oldCap), kDefaultAlign, tag_);
    }

    u64* hashes_            = nullptr; ///< 0 = empty slot
    Entry* entries_         = nullptr;
    usize size_             = 0;
    usize cap_              = 0; ///< power of two or 0
    Allocator const* alloc_ = nullptr;
    Tag tag_                = Tag::Core;
};

} // namespace kiln
