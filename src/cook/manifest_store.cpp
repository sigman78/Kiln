// The writer of a store (manifest_store.h). The writer's profile lives in memory as entries and
// input records, each behind a chained name index; other profiles are kept as they were read.
// commit_manifest() writes manifest.dir, then manifest.in.
#include "manifest_store.h"

#include "kiln/log.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
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

constexpr u32 kInputsMagic = fourcc('K', 'M', 'I', 'N');
constexpr u16 kInputsMajor = 0;
constexpr u16 kInputsMinor = 1;

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
    void clear() noexcept {
        heads.clear();
        next.clear();
    }
};

u64 entry_hash(AssetKind kind, StrView name) noexcept { return hash_combine(hash_name(name), u64(kind)); }

struct Entry {
    u32 nameOff = 0, nameLen = 0; ///< into ManifestStore::names
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
    bool fresh     = false;     ///< checked or cooked by this process (not written)
    u32 srcOff = 0, srcLen = 0; ///< where the fresh unit's source was found (not written)
    u64 hostDigest = 0;
    Vec<UnitInput> inputs; ///< names into `strings`; no paths
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

/// A profile another writer edits: kept as read, with its records still encoded (without their
/// profile name, which the commit writes).
struct OtherEntry {
    u32 nameOff = 0, nameLen = 0; ///< into OtherProfile::strings
    AssetKind kind = AssetKind::Mesh;
    Hash128 key;
    Hash128 checksum;
    u64 bytes = 0;
};

struct OtherProfile {
    Vec<char> strings;
    u32 nameOff = 0, nameLen = 0;
    u64 hash = 0, blockFormats = 0;
    Vec<OtherEntry> entries;
    Vec<u8> records; ///< encoded records, one after another
    Vec<u32> recordEnds;

    explicit OtherProfile(Allocator const* a) noexcept
        : strings(a, Tag::Cook), entries(a, Tag::Cook), records(a, Tag::Cook), recordEnds(a, Tag::Cook) {}
    [[nodiscard]] StrView name() const noexcept { return {strings.data() + nameOff, nameLen}; }
};

} // namespace

struct ManifestStore {
    Allocator const* alloc;
    std::mutex mutex;
    std::mutex commitMutex; ///< taken before `mutex`, never after
    std::chrono::steady_clock::time_point lastCommit;
    FileLock lock;
    Vec<char> storeDir; ///< NUL-terminated
    TargetProfile target;
    Vec<char> profileName;
    u64 targetHash = 0;

    // The writer's profile.
    Vec<char> names; ///< entry names, append-only
    Vec<Entry> entries;
    NameIndex entryIndex;
    Vec<Record> records;
    NameIndex recordIndex;
    Vec<OtherProfile> others;
    bool dirty = false;

    explicit ManifestStore(Allocator const* a) noexcept
        : alloc(a), storeDir(a, Tag::Cook), profileName(a, Tag::Cook), names(a, Tag::Cook),
          entries(a, Tag::Cook), entryIndex(a), records(a, Tag::Cook), recordIndex(a), others(a, Tag::Cook) {}

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
    [[nodiscard]] Record* live_record(StrView name) noexcept {
        u32 const i = find_record(name);
        return i == kInvalid || !records[i].live ? nullptr : &records[i];
    }
};

namespace {

// ---------------------------------------------------------------------------
// Input records: manifest.in (cook only; losing it costs only time). A header (magic, version,
// record count), then each record as its profile name and its body, then an XXH3-128 of it all.
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

/// One record's body: unit name, kind, digest, inputs, outputs.
void encode_record(Out& o, Record const& r) noexcept {
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

/// Decodes one record's body into `r`; false when it does not decode.
bool decode_record(In& in, Record& r) noexcept {
    StrView const name = in.str();
    u64 const kind     = in.u(1);
    (void)in.u(1);
    u64 const digest   = in.u(8);
    u64 const nInputs  = in.u(2);
    u64 const nOutputs = in.u(2);
    if (!in.ok || kind > u64(AssetKind::Texture) || check_asset_name(name)) return false;
    r.nameOff    = r.add(name);
    r.nameLen    = u32(name.size);
    r.kind       = AssetKind(kind);
    r.hostDigest = digest;
    r.live       = true;
    for (u64 i = 0; i < nInputs && in.ok; ++i) {
        UnitInput x;
        u64 const role  = in.u(1);
        x.present       = in.u(1) != 0;
        StrView const n = in.str();
        x.stat.size     = in.u(8);
        x.stat.mtimeNs  = in.u(8);
        x.content       = in.hash();
        if (role < u64(InputRole::Source) || role > u64(InputRole::Buffer)) return false;
        x.role    = InputRole(role);
        x.nameOff = r.add(n);
        x.nameLen = u32(n.size);
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
    return in.ok;
}

void clear_records(ManifestStore& s) noexcept {
    s.records.clear();
    s.recordIndex.clear();
    for (OtherProfile& o : s.others) {
        o.records.clear();
        o.recordEnds.clear();
    }
}

/// Splits `bytes` (a manifest.in) among the profiles: the writer's records are decoded unless
/// `ownDropped`; other profiles keep theirs encoded; records of profiles the manifest no longer has
/// go. False (and no records) when the file does not decode.
bool load_records(ManifestStore& s, Span<u8 const> bytes, bool ownDropped) noexcept {
    if (bytes.size < 16 + 12) return false;
    Span<u8 const> const body(bytes.data, bytes.size - 16);
    Hash128 stored;
    std::memcpy(stored.bytes, body.end(), 16);
    if (!(xxh3_128(body) == stored)) return false;
    In in{body};
    if (in.u(4) != kInputsMagic || in.u(2) != kInputsMajor || in.u(2) != kInputsMinor) return false;
    u64 const count = in.u(4);
    (void)in.u(4);
    for (u64 k = 0; k < count && in.ok; ++k) {
        StrView const profile = in.str();
        usize const start     = in.at;
        Record r(s.alloc);
        if (!decode_record(in, r)) return false;
        if (profile == s.profile()) {
            if (ownDropped) continue;
            if (s.find_record(r.name()) != kInvalid) return false;
            StrView const name = r.name();
            Record& slotRec    = s.record_slot(name);
            slotRec            = std::move(r);
            continue;
        }
        for (OtherProfile& o : s.others) {
            if (o.name() != profile) continue;
            o.records.append(Span<u8 const>(in.b.data + start, in.at - start));
            o.recordEnds.push_back(u32(o.records.size()));
            break;
        }
    }
    return in.ok && in.at == in.b.size;
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
Status publish_artifact(StrView storeDir, Hash128 const& key, Span<u8 const> bytes, Hash128 const& checksum,
                        StrView name, Allocator const* alloc, DiagSink const* diag) noexcept {
    char path[1024];
    usize const n = artifact_file_path(storeDir, key, path, sizeof path);
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

/// Loads `<store>/manifest.dir`, then `<store>/manifest.in`, if there are.
Status load_manifest(ManifestStore& s, DiagSink const* diag) noexcept {
    char path[1024];
    if (manifest_file_path(s.dir(), path, sizeof path) >= sizeof path - 1)
        return make_status(Code::InvalidArgument);
    if (!io_file_exists(StrView(path))) return kOk;
    Vec<u8> bytes(s.alloc, Tag::Cook);
    KILN_TRY(io_read_file(compat_io_backend(), StrView(path), s.alloc, &bytes));
    Result<ManifestView> v = ManifestView::open(bytes.span(), diag, StrView(path));
    if (v.failed()) return v.status();

    bool ownDropped = false;
    for (u32 p = 0; p < v->profile_count(); ++p) {
        ManifestProfile const prof = v->profile(p);
        if (prof.name() == s.profile()) {
            // Entries of another definition of the profile would pass the runtime's format check
            // without being checked: this profile starts empty (the artifacts stay).
            if (prof.hash() != s.targetHash) {
                KILN_INFO(
                    "cook",
                    "%s: profile '%.*s' was cooked for another definition of it; its entries are dropped",
                    path, KILN_SV(s.profile()));
                ownDropped = true;
                s.dirty    = true;
                continue;
            }
            for (u64 i = 0; i < prof.size(); ++i) {
                ManifestEntry const e = prof.entry(i);
                s.put_entry(e.kind, e.name, e.key, e.checksum, e.bytes);
            }
            continue;
        }
        s.others.push_back(OtherProfile(s.alloc));
        OtherProfile& o = s.others.back();
        o.nameOff       = 0;
        o.nameLen       = u32(prof.name().size);
        o.strings.append(Span<char const>(prof.name().data, prof.name().size));
        o.hash         = prof.hash();
        o.blockFormats = prof.block_formats();
        for (u64 i = 0; i < prof.size(); ++i) {
            ManifestEntry const e = prof.entry(i);
            OtherEntry oe;
            oe.nameOff  = u32(o.strings.size());
            oe.nameLen  = u32(e.name.size);
            oe.kind     = e.kind;
            oe.key      = e.key;
            oe.checksum = e.checksum;
            oe.bytes    = e.bytes;
            o.strings.append(Span<char const>(e.name.data, e.name.size));
            o.entries.push_back(oe);
        }
    }

    format(path, sizeof path, "%.*s/%s", KILN_SV(s.dir()), kInputRecordsFile);
    if (!io_file_exists(StrView(path))) return kOk;
    bytes.clear();
    if (io_read_file(compat_io_backend(), StrView(path), s.alloc, &bytes).ok() &&
        load_records(s, bytes.span(), ownDropped))
        return kOk;
    clear_records(s);
    KILN_INFO("cook", "%s is not readable; the inputs of the entries are checked again", path);
    return kOk;
}

bool name_less(StrView a, StrView b) noexcept {
    usize const n = min(a.size, b.size);
    int const c   = n ? std::memcmp(a.data, b.data, n) : 0;
    return c != 0 ? c < 0 : a.size < b.size;
}

} // namespace

Status open_manifest_store(ManifestStoreDesc const& d, ManifestStore** out) noexcept {
    KILN_VERIFY(d.target);
    *out = nullptr;
    if (char const* why = check_profile_name(d.target->name))
        return diagf(d.diag, make_status(Code::InvalidArgument), kDiagManifestName, Severity::Error,
                     d.target->name, "store", "bad profile name: %s", why);
    char path[1024];

    Allocator const* alloc = d.alloc ? d.alloc : default_allocator();
    ManifestStore* s       = new_object<ManifestStore>(alloc, Tag::Cook, alloc);
    s->storeDir.append(Span<char const>(d.storeDir.data, d.storeDir.size));
    s->storeDir.push_back('\0');
    s->target = *d.target;
    s->profileName.append(Span<char const>(d.target->name.data, d.target->name.size));
    s->target.name = s->profile();
    s->targetHash  = hash_target(s->target);

    auto const fail = [s](Status st) noexcept {
        close_manifest_store(s);
        return st;
    };
    format(path, sizeof path, "%.*s", KILN_SV(s->dir()));
    if (!make_dirs(path))
        return fail(diagf(d.diag, make_status(Code::IoError, u16(errno & 0xFFFF)), 0, Severity::Error,
                          d.storeDir, "mkdir", "cannot create %s", path));
    format(path, sizeof path, "%.*s/%s", KILN_SV(s->dir()), kStoreLockFile);
    if (Status const st = s->lock.acquire(path); st.failed())
        return fail(diagf(d.diag, st, st.code == Code::Busy ? u32(kDiagStoreLocked) : 0u, Severity::Error,
                          d.storeDir, "store",
                          st.code == Code::Busy ? "another process writes the store (%s)"
                                                : "cannot lock the store (%s)",
                          path));
    if (Status const st = load_manifest(*s, d.diag); st.failed()) return fail(st);
    *out = s;
    return kOk;
}

void close_manifest_store(ManifestStore* s) noexcept {
    if (!s) return;
    s->lock.release();
    delete_object(s->alloc, s, Tag::Cook);
}

Status publish_unit(ManifestStore* s, CookUnit& unit, u64 hostDigest, DiagSink const* diag) noexcept {
    if (unit.outputs.empty() || unit.inputs.empty()) return make_status(Code::InvalidArgument);
    if (unit.outputs[0].status.failed()) return unit.outputs[0].status;

    // Artifacts first, outside the lock: a crash before the manifest is rewritten leaves only
    // unreferenced files.
    Vec<Hash128> checksums(s->alloc, Tag::Cook);
    checksums.resize(unit.outputs.size());
    for (usize i = 0; i < unit.outputs.size(); ++i) {
        UnitOutput& o = unit.outputs[i];
        if (o.status.failed()) continue;
        checksums[i] = xxh3_128(o.bytes.span());
        o.status =
            publish_artifact(s->dir(), o.key, o.bytes.span(), checksums[i], unit.name(o), s->alloc, diag);
    }
    if (unit.outputs[0].status.failed()) return unit.outputs[0].status;

    std::lock_guard<std::mutex> const lock(s->mutex);
    StrView const unitName = unit.name(unit.outputs[0]);
    Record& r              = s->record_slot(unitName);
    // Entries of the last cook that this one did not make leave the manifest.
    if (r.live)
        for (RecordOutput const& old : r.outputs)
            if (!unit.find(old.kind, r.str(old.nameOff, old.nameLen)))
                s->remove_entry(old.kind, r.str(old.nameOff, old.nameLen));

    Record next(s->alloc);
    next.nameOff             = next.add(unitName);
    next.nameLen             = u32(unitName.size);
    next.kind                = unit.outputs[0].kind;
    next.live                = true;
    next.fresh               = true;
    StrView const sourcePath = unit.str(unit.inputs[0].pathOff, unit.inputs[0].pathLen);
    next.srcOff              = next.add(sourcePath);
    next.srcLen              = u32(sourcePath.size);
    next.hostDigest          = hostDigest;
    for (UnitInput in : unit.inputs) {
        in.nameOff = next.add(unit.str(in.nameOff, in.nameLen));
        in.pathOff = in.pathLen = 0;
        next.inputs.push_back(in);
    }
    for (usize i = 0; i < unit.outputs.size(); ++i) {
        UnitOutput const& o = unit.outputs[i];
        StrView const name  = unit.name(o);
        // A failed output stays in the record without an entry, so the record is not current.
        if (o.status.failed())
            s->remove_entry(o.kind, name);
        else
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

Status commit_manifest(ManifestStore* s, DiagSink const* diag, u32 minIntervalMs) noexcept {
    // One commit at a time, so renames land in order; the store mutex is held only for the snapshot.
    std::lock_guard<std::mutex> const commitLock(s->commitMutex);
    auto const now = std::chrono::steady_clock::now();
    Vec<u8> manifest(s->alloc, Tag::Cook);
    Vec<u8> records(s->alloc, Tag::Cook);
    {
        std::lock_guard<std::mutex> const lock(s->mutex);
        if (!s->dirty || now - s->lastCommit < std::chrono::milliseconds(minIntervalMs)) return kOk;

        // Profiles in name order: the writer's, then the others as they were read.
        Vec<ManifestEntry> mine(s->alloc, Tag::Cook);
        for (Entry const& e : s->entries)
            if (e.live) mine.push_back({s->name_of(e), e.kind, e.key, e.checksum, e.bytes});
        Vec<Vec<ManifestEntry>> otherEntries(s->alloc, Tag::Cook);
        Vec<ManifestProfileDesc> profiles(s->alloc, Tag::Cook);
        profiles.push_back({.name         = s->profile(),
                            .hash         = s->targetHash,
                            .blockFormats = s->target.blockFormats,
                            .entries      = mine.span()});
        for (OtherProfile const& o : s->others) {
            otherEntries.push_back(Vec<ManifestEntry>(s->alloc, Tag::Cook));
            Vec<ManifestEntry>& es = otherEntries.back();
            for (OtherEntry const& e : o.entries)
                es.push_back(
                    {StrView(o.strings.data() + e.nameOff, e.nameLen), e.kind, e.key, e.checksum, e.bytes});
        }
        for (usize i = 0; i < s->others.size(); ++i)
            profiles.push_back({.name         = s->others[i].name(),
                                .hash         = s->others[i].hash,
                                .blockFormats = s->others[i].blockFormats,
                                .entries      = otherEntries[i].span()});
        Vec<u32> order(s->alloc, Tag::Cook);
        for (u32 i = 0; i < profiles.size(); ++i)
            order.push_back(i);
        std::sort(order.begin(), order.end(), [&profiles](u32 a, u32 b) noexcept {
            return name_less(profiles[a].name, profiles[b].name);
        });

        KILN_TRY(write_manifest({.profiles = profiles.span()}, &manifest, diag));

        // Records, by profile in name order, then (the writer's) by unit name: the same content
        // always gives the same bytes.
        Out o{records};
        o.u(kInputsMagic, 4);
        o.u(kInputsMajor, 2);
        o.u(kInputsMinor, 2);
        usize const countAt = records.size();
        o.u(0, 4);
        o.u(0, 4);
        u64 count = 0;
        for (u32 const p : order) {
            if (p == 0) {
                Vec<u32> recs(s->alloc, Tag::Cook);
                for (u32 i = 0; i < s->records.size(); ++i)
                    if (s->records[i].live) recs.push_back(i);
                std::sort(recs.begin(), recs.end(), [s](u32 a, u32 b) noexcept {
                    return name_less(s->records[a].name(), s->records[b].name());
                });
                for (u32 const i : recs) {
                    o.str(s->profile());
                    encode_record(o, s->records[i]);
                    ++count;
                }
                continue;
            }
            OtherProfile const& other = s->others[p - 1];
            u32 start                 = 0;
            for (u32 const end : other.recordEnds) {
                o.str(other.name());
                records.append(Span<u8 const>(other.records.data() + start, end - start));
                start = end;
                ++count;
            }
        }
        write_unaligned<u32>(records.data() + countAt, u32(count));
        o.hash(xxh3_128(records.span()));
        s->dirty      = false;
        s->lastCommit = now;
    }
    char path[1024];
    (void)manifest_file_path(s->dir(), path, sizeof path);
    Status st = replace_file(path, manifest.span(), diag);
    // The records follow the manifest: a crash in between costs a re-check, never a wrong entry.
    if (st.ok()) {
        format(path, sizeof path, "%.*s/%s", KILN_SV(s->dir()), kInputRecordsFile);
        st = replace_file(path, records.span(), diag);
    }
    if (st.failed()) {
        std::lock_guard<std::mutex> const lock(s->mutex);
        s->dirty = true; // written next time
    }
    return st;
}

bool manifest_find(ManifestStore* s, AssetKind kind, StrView name, Hash128* key) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    u32 const i = s->find_entry(kind, name);
    if (i == kInvalid || !s->entries[i].live) return false;
    *key = s->entries[i].key;
    return true;
}

bool copy_input_record(ManifestStore* s, StrView name, CookUnit* out, u64* hostDigest) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    Record const* r = s->live_record(name);
    if (!r) return false;
    out->inputs.clear();
    out->outputs.clear();
    out->strings.clear();
    out->strings.append(r->strings.span());
    out->inputs.append(r->inputs.span());
    for (RecordOutput const& ro : r->outputs) {
        UnitOutput o;
        o.nameOff   = ro.nameOff;
        o.nameLen   = ro.nameLen;
        o.kind      = ro.kind;
        o.slot      = ro.slot;
        u32 const e = s->find_entry(ro.kind, r->str(ro.nameOff, ro.nameLen));
        if (e == kInvalid || !s->entries[e].live)
            o.status = make_status(Code::NotFound);
        else
            o.key = s->entries[e].key;
        out->outputs.push_back(std::move(o));
    }
    *hostDigest = r->hostDigest;
    return true;
}

void set_record_digest(ManifestStore* s, StrView name, u64 hostDigest) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    Record* r = s->live_record(name);
    if (!r || r->hostDigest == hostDigest) return;
    r->hostDigest = hostDigest;
    s->dirty      = true;
}

void set_record_stats(ManifestStore* s, StrView name, CookUnit const& rec) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    Record* r = s->live_record(name);
    if (!r || r->inputs.size() != rec.inputs.size()) return;
    for (usize i = 0; i < rec.inputs.size(); ++i)
        r->inputs[i].stat = rec.inputs[i].stat;
    s->dirty = true;
}

bool record_is_current(ManifestStore* s, UnitDesc const& d, u64 hostDigest, bool rehash) noexcept {
    CookUnit rec(d.env.alloc ? d.env.alloc : s->alloc);
    u64 digest = 0;
    if (!copy_input_record(s, d.name, &rec, &digest) || rec.inputs.empty()) return false;
    // Every output needs its entry and its artifact: a deleted manifest or artifact cooks again.
    for (UnitOutput const& o : rec.outputs) {
        if (o.status.failed()) return false;
        char path[1024];
        usize const n = artifact_file_path(s->dir(), o.key, path, sizeof path);
        if (n >= sizeof path - 1 || !io_file_exists(StrView(path, n))) return false;
    }
    InputsCheck const inputs = check_recorded_inputs(rec, d.sourcePath, rehash);
    if (inputs == InputsCheck::Changed) return false;
    if (inputs == InputsCheck::Touched) set_record_stats(s, d.name, rec);
    if (digest == hostDigest) return true;
    if (!recorded_keys_match(d, rec)) return false;
    set_record_digest(s, d.name, hostDigest);
    return true;
}

bool is_fresh(ManifestStore* s, StrView name) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    Record const* r = s->live_record(name);
    return r && r->fresh;
}

void mark_fresh(ManifestStore* s, StrView name, StrView sourcePath) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    Record* r = s->live_record(name);
    if (!r) return;
    r->fresh = true;
    if (r->str(r->srcOff, r->srcLen) == sourcePath) return;
    r->srcOff = r->add(sourcePath);
    r->srcLen = u32(sourcePath.size);
}

void fresh_units(ManifestStore* s, Vec<char>* out) noexcept {
    std::lock_guard<std::mutex> const lock(s->mutex);
    out->clear();
    for (Record const& r : s->records) {
        if (!r.live || !r.fresh) continue;
        out->append(Span<char const>(r.name().data, r.name().size));
        out->push_back('\0');
        StrView const src = r.str(r.srcOff, r.srcLen);
        out->append(Span<char const>(src.data, src.size));
        out->push_back('\0');
    }
}

} // namespace kiln::cook
