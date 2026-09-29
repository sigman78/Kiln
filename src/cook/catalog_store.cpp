// The catalog writer of one profile (catalog_store.h). Entries and input records live in memory,
// each behind a chained name index; commit_catalog() writes both files whole.
#include "catalog_store.h"

#include "kiln/log.h"

#include <algorithm>
#include <cerrno>
#include <mutex>

#if defined(KILN_OS_WINDOWS)
#include <direct.h>  // _mkdir
#include <windows.h> // CreateFileW; WIN32_LEAN_AND_MEAN/NOMINMAX set by kiln_apply_defaults
#else
#include <fcntl.h>    // open
#include <sys/file.h> // flock
#include <sys/stat.h> // mkdir
#include <unistd.h>   // close
#endif

namespace kiln::cook {

namespace {

constexpr u32 kInputsMagic         = fourcc('K', 'K', 'I', 'N');
constexpr u16 kInputsMajor         = 0;
constexpr u16 kInputsMinor         = 1;
constexpr char kNamedStoreMarker[] = "kiln-store.txt";

// ---------------------------------------------------------------------------
// Paths and the lock
// ---------------------------------------------------------------------------

bool mkdir_one(char const* path) noexcept {
#if defined(KILN_OS_WINDOWS)
    if (_mkdir(path) == 0) return true;
#else
    if (::mkdir(path, 0755) == 0) return true;
#endif
    return errno == EEXIST;
}

/// mkdir -p over the NUL-terminated forward-slash path in `buf`.
bool make_dirs(char* buf) noexcept {
    usize const n = std::strlen(buf);
    for (usize i = 1; i <= n; ++i) {
        if (i < n && buf[i] != '/') continue;
        if (i == 2 && buf[1] == ':') continue; // a bare drive letter
        char const saved = buf[i];
        buf[i]           = '\0';
        bool const ok    = mkdir_one(buf);
        buf[i]           = saved;
        if (!ok) return false;
    }
    return true;
}

/// An exclusive lock on a file, held until release() or the end of the process.
struct FileLock {
#if defined(KILN_OS_WINDOWS)
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif

    /// Busy when another holder has it.
    Status acquire(char const* path) noexcept {
#if defined(KILN_OS_WINDOWS)
        wchar_t wpath[1024];
        if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024) == 0)
            return make_status(Code::InvalidArgument);
        h = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
        if (h != INVALID_HANDLE_VALUE) return kOk;
        DWORD const err = GetLastError();
        return make_status(err == ERROR_SHARING_VIOLATION ? Code::Busy : Code::IoError, u16(err & 0xFFFFu));
#else
        fd = ::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return make_status(Code::IoError, u16(errno & 0xFFFF));
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return kOk;
        int const e = errno;
        ::close(fd);
        fd = -1;
        return make_status(e == EWOULDBLOCK ? Code::Busy : Code::IoError, u16(e & 0xFFFF));
#endif
    }

    void release() noexcept {
#if defined(KILN_OS_WINDOWS)
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
#else
        if (fd >= 0) ::close(fd);
        fd = -1;
#endif
    }
};

// ---------------------------------------------------------------------------
// In-memory tables
// ---------------------------------------------------------------------------

/// Items found by a name hash; items with the same hash chain through `next`.
struct NameIndex {
    HashMap<u64, u32> heads;
    Vec<u32> next;

    NameIndex(Allocator const* a) noexcept : heads(a, Tag::Cook), next(a, Tag::Cook) {}

    template <class Match> [[nodiscard]] u32 find(u64 h, Match&& match) const noexcept {
        u32 const* head = heads.find(h);
        for (u32 i = head ? *head : kInvalid; i != kInvalid; i = next[i])
            if (match(i)) return i;
        return kInvalid;
    }
    /// Item `i` (the next one, numbered in order) gets hash `h`.
    void add(u64 h, u32 i) noexcept {
        KILN_ASSERT(i == next.size());
        u32* head = heads.find(h);
        next.push_back(head ? *head : kInvalid);
        heads.insert(h, i);
    }
};

u64 entry_hash(AssetKind kind, StrView name) noexcept { return hash_combine(hash_name(name), u64(kind)); }

struct Entry {
    u32 nameOff = 0, nameLen = 0; ///< into CatalogStore::names
    AssetKind kind = AssetKind::Mesh;
    bool live      = false; ///< false: removed; the slot stays in the index
    Hash128 key;
    Hash128 checksum;
    u64 bytes = 0;
};

struct RecordOutput {
    u32 nameOff = 0, nameLen = 0; ///< into Record::strings
    AssetKind kind = AssetKind::Mesh;
    SlotHint slot  = SlotHint::None;
};

struct Record {
    Vec<char> strings;
    u32 nameOff = 0, nameLen = 0;
    AssetKind kind = AssetKind::Mesh;
    bool live      = false;
    bool fresh     = false; ///< checked or cooked by this process (not written)
    u64 hostDigest = 0;
    Vec<UnitInput> inputs; ///< strings into `strings`
    Vec<RecordOutput> outputs;

    explicit Record(Allocator const* a) noexcept
        : strings(a, Tag::Cook), inputs(a, Tag::Cook), outputs(a, Tag::Cook) {}
    [[nodiscard]] StrView str(u32 off, u32 len) const noexcept { return {strings.data() + off, len}; }
    [[nodiscard]] StrView name() const noexcept { return str(nameOff, nameLen); }
    u32 add(StrView s) noexcept {
        u32 const off = u32(strings.size());
        strings.append(Span<char const>(s.data, s.size));
        return off;
    }
};

} // namespace

struct CatalogStore {
    Allocator const* alloc;
    std::mutex mutex;
    FileLock lock;
    Vec<char> storeDir; ///< NUL-terminated
    TargetProfile target;
    Vec<char> profileName;
    u64 targetHash = 0;

    Vec<char> names; ///< entry names, append-only
    Vec<Entry> entries;
    NameIndex entryIndex;
    Vec<Record> records;
    NameIndex recordIndex;
    bool dirty = false;

    explicit CatalogStore(Allocator const* a) noexcept
        : alloc(a), storeDir(a, Tag::Cook), profileName(a, Tag::Cook), names(a, Tag::Cook),
          entries(a, Tag::Cook), entryIndex(a), records(a, Tag::Cook), recordIndex(a) {}

    [[nodiscard]] StrView dir() const noexcept { return {storeDir.data(), storeDir.size() - 1}; }
    [[nodiscard]] StrView profile() const noexcept { return {profileName.data(), profileName.size()}; }
    [[nodiscard]] StrView name_of(Entry const& e) const noexcept {
        return {names.data() + e.nameOff, e.nameLen};
    }

    [[nodiscard]] u32 find_entry(AssetKind kind, StrView name) const noexcept {
        return entryIndex.find(entry_hash(kind, name),
                               [&](u32 i) { return entries[i].kind == kind && name_of(entries[i]) == name; });
    }
    /// The live entry of (`kind`, `name`) with these values; a new one or a revived slot.
    void put_entry(AssetKind kind, StrView name, Hash128 const& key, Hash128 const& checksum,
                   u64 bytes) noexcept {
        u32 i = find_entry(kind, name);
        if (i == kInvalid) {
            i = u32(entries.size());
            Entry e;
            e.nameOff = u32(names.size());
            e.nameLen = u32(name.size);
            e.kind    = kind;
            names.append(Span<char const>(name.data, name.size));
            entries.push_back(e);
            entryIndex.add(entry_hash(kind, name), i);
        }
        Entry& e   = entries[i];
        e.live     = true;
        e.key      = key;
        e.checksum = checksum;
        e.bytes    = bytes;
    }
    void remove_entry(AssetKind kind, StrView name) noexcept {
        u32 const i = find_entry(kind, name);
        if (i != kInvalid) entries[i].live = false;
    }

    [[nodiscard]] u32 find_record(StrView name) const noexcept {
        return recordIndex.find(hash_name(name), [&](u32 i) { return records[i].name() == name; });
    }
    /// The record slot of `name`, made if missing (its old contents stay until replaced).
    Record& record_slot(StrView name) noexcept {
        u32 i = find_record(name);
        if (i == kInvalid) {
            i = u32(records.size());
            records.push_back(Record(alloc));
            recordIndex.add(hash_name(name), i);
        }
        return records[i];
    }
};

namespace {

// ---------------------------------------------------------------------------
// Input records file: inputs/<profile>.kin (cook-side only; losing it costs only time)
// ---------------------------------------------------------------------------

struct Out {
    Vec<u8>& b;
    void u(u64 v, usize n) noexcept {
        for (usize i = 0; i < n; ++i)
            b.push_back(u8(v >> (8 * i)));
    }
    void str(StrView s) noexcept {
        u(s.size, 2);
        b.append(Span<u8 const>(reinterpret_cast<u8 const*>(s.data), s.size));
    }
    void hash(Hash128 const& h) noexcept { b.append(Span<u8 const>(h.bytes, 16)); }
};

struct In {
    Span<u8 const> b;
    usize at = 0;
    bool ok  = true;

    u64 u(usize n) noexcept {
        if (!ok || b.size - at < n) return ok = false, 0;
        u64 v = 0;
        for (usize i = 0; i < n; ++i)
            v |= u64(b.data[at + i]) << (8 * i);
        at += n;
        return v;
    }
    StrView str() noexcept {
        usize const n = usize(u(2));
        if (!ok || b.size - at < n) return ok = false, StrView();
        StrView const s(reinterpret_cast<char const*>(b.data + at), n);
        at += n;
        return s;
    }
    Hash128 hash() noexcept {
        Hash128 h;
        if (!ok || b.size - at < 16) return ok = false, h;
        std::memcpy(h.bytes, b.data + at, 16);
        at += 16;
        return h;
    }
};

void encode_records(CatalogStore const& s, Vec<u8>& bytes) noexcept {
    Vec<u32> order(s.alloc, Tag::Cook);
    for (u32 i = 0; i < s.records.size(); ++i)
        if (s.records[i].live) order.push_back(i);
    std::sort(order.begin(), order.end(), [&s](u32 a, u32 b) noexcept {
        StrView const x = s.records[a].name(), y = s.records[b].name();
        usize const n = min(x.size, y.size);
        int const c   = n ? std::memcmp(x.data, y.data, n) : 0;
        return c != 0 ? c < 0 : x.size < y.size;
    });
    bytes.clear();
    Out o{bytes};
    o.u(kInputsMagic, 4);
    o.u(kInputsMajor, 2);
    o.u(kInputsMinor, 2);
    o.u(order.size(), 4);
    o.u(0, 4);
    for (u32 const i : order) {
        Record const& r = s.records[i];
        o.str(r.name());
        o.u(u8(r.kind), 1);
        o.u(0, 1);
        o.u(r.hostDigest, 8);
        o.u(r.inputs.size(), 2);
        o.u(r.outputs.size(), 2);
        for (UnitInput const& in : r.inputs) {
            o.u(u8(in.role), 1);
            o.u(in.present ? 1 : 0, 1);
            o.str(r.str(in.nameOff, in.nameLen));
            o.str(r.str(in.pathOff, in.pathLen));
            o.u(in.stat.size, 8);
            o.u(in.stat.mtimeNs, 8);
            o.hash(in.content);
        }
        for (RecordOutput const& out : r.outputs) {
            o.u(u8(out.kind), 1);
            o.u(u8(out.slot), 1);
            o.str(r.str(out.nameOff, out.nameLen));
        }
    }
    o.hash(xxh3_128(bytes.span()));
}

/// Replaces the records with those in `bytes`; false (and no records) if they do not decode.
bool decode_records(CatalogStore& s, Span<u8 const> bytes) noexcept {
    if (bytes.size < 32) return false;
    Span<u8 const> const body(bytes.data, bytes.size - 16);
    Hash128 stored;
    std::memcpy(stored.bytes, body.end(), 16);
    if (!(xxh3_128(body) == stored)) return false;
    In in{body};
    if (in.u(4) != kInputsMagic || in.u(2) != kInputsMajor || in.u(2) != kInputsMinor) return false;
    u64 const count = in.u(4);
    (void)in.u(4);
    for (u64 k = 0; k < count && in.ok; ++k) {
        StrView const name = in.str();
        u64 const kind     = in.u(1);
        (void)in.u(1);
        u64 const digest   = in.u(8);
        u64 const nInputs  = in.u(2);
        u64 const nOutputs = in.u(2);
        if (!in.ok || kind > u64(AssetKind::Texture) || check_asset_name(name)) return false;
        Record r(s.alloc);
        r.nameOff    = r.add(name);
        r.nameLen    = u32(name.size);
        r.kind       = AssetKind(kind);
        r.hostDigest = digest;
        r.live       = true;
        for (u64 i = 0; i < nInputs && in.ok; ++i) {
            UnitInput x;
            u64 const role  = in.u(1);
            x.present       = in.u(1) != 0;
            StrView const n = in.str(), p = in.str();
            x.stat.size    = in.u(8);
            x.stat.mtimeNs = in.u(8);
            x.content      = in.hash();
            if (role < u64(InputRole::Source) || role > u64(InputRole::Buffer)) return false;
            x.role    = InputRole(role);
            x.nameOff = r.add(n);
            x.nameLen = u32(n.size);
            x.pathOff = r.add(p);
            x.pathLen = u32(p.size);
            r.inputs.push_back(x);
        }
        for (u64 i = 0; i < nOutputs && in.ok; ++i) {
            RecordOutput x;
            u64 const k2    = in.u(1);
            u64 const slot  = in.u(1);
            StrView const n = in.str();
            if (k2 > u64(AssetKind::Texture) || slot > u64(SlotHint::Emissive) || check_asset_name(n))
                return false;
            x.kind    = AssetKind(k2);
            x.slot    = SlotHint(slot);
            x.nameOff = r.add(n);
            x.nameLen = u32(n.size);
            r.outputs.push_back(x);
        }
        if (!in.ok || s.find_record(name) != kInvalid) return false;
        s.record_slot(name) = std::move(r);
    }
    return in.ok && in.at == in.b.size;
}

void clear_records(CatalogStore& s) noexcept {
    s.records.clear();
    s.recordIndex.heads.clear();
    s.recordIndex.next.clear();
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

/// Writes `bytes` to the file `path` by a temporary file and a rename.
Status replace_file(char const* path, Span<u8 const> bytes, DiagSink const* diag) noexcept {
    StrView const p(path);
    usize const slash = p.rfind('/');
    KILN_VERIFY(slash != StrView::kNpos);
    return store_write(p.substr(0, slash), p.substr(slash + 1), bytes, diag, true);
}

/// The artifact of `key`: written if absent; an existing one must have the same bytes.
Status publish_artifact(StrView storeDir, AssetKind kind, Hash128 const& key, Span<u8 const> bytes,
                        Hash128 const& checksum, StrView name, Allocator const* alloc,
                        DiagSink const* diag) noexcept {
    char path[1024];
    usize const n = artifact_file_path(storeDir, kind, key, path, sizeof path);
    if (n >= sizeof path - 1) return make_status(Code::InvalidArgument);
    StrView const pathView(path, n);
    if (io_file_exists(pathView)) {
        Vec<u8> have(alloc, Tag::Cook);
        Status const st = io_read_file(compat_io_backend(), pathView, alloc, &have);
        if (st.failed())
            return diagf(diag, st, 0, Severity::Error, name, "store", "cannot read the artifact %s", path);
        if (have.size() == bytes.size && xxh3_128(have.span()) == checksum) return kOk;
        return diagf(
            diag, make_status(Code::ValidationFailed), kDiagNondeterministicCook, Severity::Error, name,
            "store",
            "the cook gave other bytes for the same build key than %s: the cook is not deterministic. "
            "The artifact keeps its bytes",
            path);
    }
    usize const slash = pathView.rfind('/');
    path[slash]       = '\0';
    if (!make_dirs(path))
        return diagf(diag, make_status(Code::IoError, u16(errno & 0xFFFF)), 0, Severity::Error, name, "mkdir",
                     "cannot create %s", path);
    return store_write(StrView(path, slash), pathView.substr(slash + 1), bytes, diag, false);
}

Status load_catalog(CatalogStore& s, DiagSink const* diag) noexcept {
    char path[1024];
    if (catalog_file_path(s.dir(), s.profile(), path, sizeof path) >= sizeof path - 1)
        return make_status(Code::InvalidArgument);
    if (!io_file_exists(StrView(path))) return kOk;
    Vec<u8> bytes(s.alloc, Tag::Cook);
    KILN_TRY(io_read_file(compat_io_backend(), StrView(path), s.alloc, &bytes));
    Result<CatalogView> v = CatalogView::open(bytes.span(), diag, StrView(path));
    if (v.failed()) return v.status();
    for (u64 i = 0; i < v->size(); ++i) {
        CatalogEntry const e = v->entry(i);
        s.put_entry(e.kind, e.name, e.key, e.checksum, e.bytes);
    }
    if (v->profile().hash != s.targetHash)
        KILN_INFO("cook",
                  "%s was cooked for another definition of profile '%.*s'; its entries are checked again",
                  path, KILN_SV(s.profile()));
    return kOk;
}

void load_records(CatalogStore& s) noexcept {
    char path[1024];
    if (format(path, sizeof path, "%.*s/inputs/%.*s.kin", KILN_SV(s.dir()), KILN_SV(s.profile())) >=
        sizeof path - 1)
        return;
    if (!io_file_exists(StrView(path))) return;
    Vec<u8> bytes(s.alloc, Tag::Cook);
    if (io_read_file(compat_io_backend(), StrView(path), s.alloc, &bytes).ok() &&
        decode_records(s, bytes.span()))
        return;
    clear_records(s);
    KILN_INFO("cook", "%s is not readable; the inputs of the catalog's entries are checked again", path);
}

} // namespace

Status open_catalog_store(CatalogStoreDesc const& d, CatalogStore** out) noexcept {
    KILN_VERIFY(d.target);
    *out = nullptr;
    if (char const* why = check_profile_name(d.target->name))
        return diagf(d.diag, make_status(Code::InvalidArgument), kDiagCatalogName, Severity::Error,
                     d.target->name, "store", "bad profile name for a catalog: %s", why);
    char path[1024];
    if (format(path, sizeof path, "%.*s/%s", KILN_SV(d.storeDir), kNamedStoreMarker) >= sizeof path - 1)
        return make_status(Code::InvalidArgument);
    if (io_file_exists(StrView(path)))
        return diagf(d.diag, make_status(Code::InvalidArgument), kDiagStoreProfileMismatch, Severity::Error,
                     d.storeDir, "store",
                     "the store has the named layout (%s); a catalog store needs another directory",
                     kNamedStoreMarker);

    Allocator const* alloc = d.alloc ? d.alloc : default_allocator();
    CatalogStore* s        = new_object<CatalogStore>(alloc, Tag::Cook, alloc);
    s->storeDir.append(Span<char const>(d.storeDir.data, d.storeDir.size));
    s->storeDir.push_back('\0');
    s->target = *d.target;
    s->profileName.append(Span<char const>(d.target->name.data, d.target->name.size));
    s->target.name = s->profile();
    s->targetHash  = hash_target(s->target);

    auto const fail = [s](Status st) noexcept {
        close_catalog_store(s);
        return st;
    };
    static char const* const kDirs[] = {"catalogs", "inputs", "artifacts"};
    for (char const* sub : kDirs) {
        format(path, sizeof path, "%.*s/%s", KILN_SV(s->dir()), sub);
        if (!make_dirs(path))
            return fail(diagf(d.diag, make_status(Code::IoError, u16(errno & 0xFFFF)), 0, Severity::Error,
                              d.storeDir, "mkdir", "cannot create %s", path));
    }
    format(path, sizeof path, "%.*s/catalogs/%.*s.lock", KILN_SV(s->dir()), KILN_SV(s->profile()));
    if (Status const st = s->lock.acquire(path); st.failed())
        return fail(diagf(d.diag, st, st.code == Code::Busy ? u32(kDiagCatalogLocked) : 0u, Severity::Error,
                          d.storeDir, "store",
                          st.code == Code::Busy ? "another process writes the catalog of profile '%.*s' (%s)"
                                                : "cannot lock the catalog of profile '%.*s' (%s)",
                          KILN_SV(s->profile()), path));
    if (Status const st = load_catalog(*s, d.diag); st.failed()) return fail(st);
    load_records(*s);
    *out = s;
    return kOk;
}

void close_catalog_store(CatalogStore* s) noexcept {
    if (!s) return;
    s->lock.release();
    delete_object(s->alloc, s, Tag::Cook);
}

Status publish_unit(CatalogStore* s, CookUnit& unit, u64 hostDigest, DiagSink const* diag) noexcept {
    if (unit.outputs.empty()) return make_status(Code::InvalidArgument);
    if (unit.outputs[0].status.failed()) return unit.outputs[0].status;

    // Artifacts first, outside the lock: a crash before the catalog is rewritten leaves only
    // unreferenced files.
    Vec<Hash128> checksums(s->alloc, Tag::Cook);
    checksums.resize(unit.outputs.size());
    for (usize i = 0; i < unit.outputs.size(); ++i) {
        UnitOutput& o = unit.outputs[i];
        if (o.status.failed()) continue;
        checksums[i] = xxh3_128(o.bytes.span());
        o.status     = publish_artifact(s->dir(), o.kind, o.key, o.bytes.span(), checksums[i], unit.name(o),
                                        s->alloc, diag);
    }
    if (unit.outputs[0].status.failed()) return unit.outputs[0].status;

    std::lock_guard<std::mutex> const lock(s->mutex);
    StrView const unitName = unit.name(unit.outputs[0]);
    Record& r              = s->record_slot(unitName);
    // Entries of the last cook that this one did not make leave the catalog.
    if (r.live)
        for (RecordOutput const& old : r.outputs)
            if (!unit.find(old.kind, r.str(old.nameOff, old.nameLen)))
                s->remove_entry(old.kind, r.str(old.nameOff, old.nameLen));

    Record next(s->alloc);
    next.nameOff    = next.add(unitName);
    next.nameLen    = u32(unitName.size);
    next.kind       = unit.outputs[0].kind;
    next.live       = true;
    next.fresh      = true;
    next.hostDigest = hostDigest;
    for (UnitInput in : unit.inputs) {
        StrView const n = unit.str(in.nameOff, in.nameLen), p = unit.str(in.pathOff, in.pathLen);
        in.nameOff = next.add(n);
        in.pathOff = next.add(p);
        next.inputs.push_back(in);
    }
    for (usize i = 0; i < unit.outputs.size(); ++i) {
        UnitOutput const& o = unit.outputs[i];
        StrView const name  = unit.name(o);
        if (o.status.failed()) {
            s->remove_entry(o.kind, name);
            continue;
        }
        s->put_entry(o.kind, name, o.key, checksums[i], o.bytes.size());
        RecordOutput ro;
        ro.nameOff = next.add(name);
        ro.nameLen = u32(name.size);
        ro.kind    = o.kind;
        ro.slot    = o.slot;
        next.outputs.push_back(ro);
    }
    r        = std::move(next);
    s->dirty = true;
    return kOk;
}

Status commit_catalog(CatalogStore* s, DiagSink const* diag) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    if (!s->dirty) return kOk;

    Vec<CatalogEntry> entries(s->alloc, Tag::Cook);
    for (Entry const& e : s->entries)
        if (e.live) entries.push_back({s->name_of(e), e.kind, e.key, e.checksum, e.bytes});
    Vec<u8> bytes(s->alloc, Tag::Cook);
    KILN_TRY(write_catalog(
        {
            .profile = {.name = s->profile(), .hash = s->targetHash, .blockFormats = s->target.blockFormats},
            .entries = entries.span()
    },
        &bytes, diag));
    char path[1024];
    (void)catalog_file_path(s->dir(), s->profile(), path, sizeof path);
    KILN_TRY(replace_file(path, bytes.span(), diag));

    // The records follow the catalog: a crash in between costs a re-check, never a wrong entry.
    encode_records(*s, bytes);
    format(path, sizeof path, "%.*s/inputs/%.*s.kin", KILN_SV(s->dir()), KILN_SV(s->profile()));
    KILN_TRY(replace_file(path, bytes.span(), diag));
    s->dirty = false;
    return kOk;
}

bool catalog_find(CatalogStore* s, AssetKind kind, StrView name, Hash128* key) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    u32 const i = s->find_entry(kind, name);
    if (i == kInvalid || !s->entries[i].live) return false;
    *key = s->entries[i].key;
    return true;
}

bool copy_input_record(CatalogStore* s, StrView name, CookUnit* out, u64* hostDigest) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    u32 const i = s->find_record(name);
    if (i == kInvalid || !s->records[i].live) return false;
    Record const& r = s->records[i];
    out->inputs.clear();
    out->outputs.clear();
    out->strings.clear();
    out->strings.append(r.strings.span());
    out->inputs.append(r.inputs.span());
    for (RecordOutput const& ro : r.outputs) {
        UnitOutput o;
        o.nameOff   = ro.nameOff;
        o.nameLen   = ro.nameLen;
        o.kind      = ro.kind;
        o.slot      = ro.slot;
        u32 const e = s->find_entry(ro.kind, r.str(ro.nameOff, ro.nameLen));
        if (e == kInvalid || !s->entries[e].live)
            o.status = make_status(Code::NotFound);
        else
            o.key = s->entries[e].key;
        out->outputs.push_back(std::move(o));
    }
    *hostDigest = r.hostDigest;
    return true;
}

bool is_fresh(CatalogStore* s, StrView name) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    u32 const i = s->find_record(name);
    return i != kInvalid && s->records[i].live && s->records[i].fresh;
}

void mark_fresh(CatalogStore* s, StrView name) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    u32 const i = s->find_record(name);
    if (i != kInvalid && s->records[i].live) s->records[i].fresh = true;
}

void fresh_units(CatalogStore* s, Vec<char>* out) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    out->clear();
    for (Record const& r : s->records) {
        if (!r.live || !r.fresh) continue;
        out->append(Span<char const>(r.name().data, r.name().size));
        out->push_back('\0');
    }
}

void set_record_digest(CatalogStore* s, StrView name, u64 hostDigest) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    u32 const i = s->find_record(name);
    if (i == kInvalid || !s->records[i].live || s->records[i].hostDigest == hostDigest) return;
    s->records[i].hostDigest = hostDigest;
    s->dirty                 = true;
}

} // namespace kiln::cook
