// src/cook/manifest_store.h — the writer of a store: immutable artifacts and the manifest, under the
// store's lock (docs/design/store-catalog.md). A writer edits one profile and keeps the others as
// they are. Thread-safe: cooks on several workers publish into one store.
#pragma once

#include "unit.h"

namespace kiln::cook {

struct ManifestStore;

struct ManifestStoreDesc {
    StrView storeDir            = {};
    TargetProfile const* target = nullptr; ///< the profile this writer edits
    Allocator const* alloc      = nullptr; ///< nullptr: default allocator
    DiagSink const* diag        = nullptr;
};

/// Takes `<store>/manifest.lock` (K3009 if another writer holds it), makes the store's directories
/// and loads the manifest. A malformed manifest is its K42xx. The profile's entries and records are
/// dropped when they were cooked for another definition of it (another hash_target); malformed
/// records are dropped (they only save time).
[[nodiscard]] Status open_manifest_store(ManifestStoreDesc const& d, ManifestStore** out) noexcept;
/// Releases the lock and frees the store. Does not commit.
void close_manifest_store(ManifestStore* s) noexcept;

/// Writes the artifacts of `unit`'s outputs that cooked (create-if-absent; K3010 when an existing
/// artifact of the same key has other bytes), then replaces the unit's entries and input record in
/// memory, and marks the unit fresh. An output whose artifact fails gets that Status and leaves the
/// manifest; when the first output fails, nothing changes and its Status is returned.
[[nodiscard]] Status publish_unit(ManifestStore* s, CookUnit& unit, u64 hostDigest,
                                  DiagSink const* diag) noexcept;
/// Rewrites the manifest (temporary file, then rename) if anything changed and the last commit is
/// at least `minIntervalMs` old. A failed write is tried again next time.
[[nodiscard]] Status commit_manifest(ManifestStore* s, DiagSink const* diag, u32 minIntervalMs = 0) noexcept;

/// The entry of `name` and `kind` in memory, or false.
[[nodiscard]] bool manifest_find(ManifestStore* s, AssetKind kind, StrView name, Hash128* key) noexcept;
/// Copies the input record of the unit `name` into `out`: inputs (without paths), and the outputs'
/// names, kinds, slots and current keys (no bytes). False when there is none.
[[nodiscard]] bool copy_input_record(ManifestStore* s, StrView name, CookUnit* out, u64* hostDigest) noexcept;
/// Sets the host-settings digest of the unit `name`'s record (its keys were checked again).
void set_record_digest(ManifestStore* s, StrView name, u64 hostDigest) noexcept;
/// Sets the stats of the unit `name`'s inputs to those in `rec` (their content did not change).
void set_record_stats(ManifestStore* s, StrView name, CookUnit const& rec) noexcept;

/// True if the unit `d.name`'s record still describes it: every output with its entry and
/// artifact, every input unchanged next to `d.sourcePath` (check_recorded_inputs(); new stats
/// of unchanged content are kept), and `hostDigest` or, when the digest differs, the host's
/// settings giving the recorded keys (the record then takes `hostDigest`).
[[nodiscard]] bool record_is_current(ManifestStore* s, UnitDesc const& d, u64 hostDigest,
                                     bool rehash) noexcept;

/// Session state, never written: a unit is fresh once this process checked its inputs or cooked
/// it, and remembers the source it was found at. The source poller watches the fresh units.
[[nodiscard]] bool is_fresh(ManifestStore* s, StrView name) noexcept;
void mark_fresh(ManifestStore* s, StrView name, StrView sourcePath) noexcept;
/// The fresh units into `out` (replaced): name, NUL, source path, NUL, for each.
void fresh_units(ManifestStore* s, Vec<char>* out) noexcept;

} // namespace kiln::cook
