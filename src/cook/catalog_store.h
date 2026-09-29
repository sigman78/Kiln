// src/cook/catalog_store.h — the writer of one profile's catalog in a store: immutable artifacts,
// the catalog and the input records, under the profile's lock (docs/design/store-catalog.md).
// Thread-safe: cooks on several workers publish into one store.
#pragma once

#include "unit.h"

namespace kiln::cook {

struct CatalogStore;

struct CatalogStoreDesc {
    StrView storeDir            = {};
    TargetProfile const* target = nullptr; ///< its name names the catalog
    Allocator const* alloc      = nullptr; ///< nullptr: default allocator
    DiagSink const* diag        = nullptr;
};

/// Takes `<store>/catalogs/<profile>.lock` (K3009 if another writer holds it), makes the store's
/// directories, and loads the profile's catalog and input records. A named-layout store is K3008;
/// a malformed catalog is its K42xx; malformed input records are dropped (they only save time).
[[nodiscard]] Status open_catalog_store(CatalogStoreDesc const& d, CatalogStore** out) noexcept;
/// Releases the lock and frees the store. Does not commit.
void close_catalog_store(CatalogStore* s) noexcept;

/// Writes the artifacts of `unit`'s outputs that cooked (create-if-absent; K3010 when an existing
/// artifact of the same key has other bytes), then replaces the unit's catalog entries and input
/// record in memory. An output whose artifact fails gets that Status and leaves the catalog; when
/// the first output fails, nothing changes and its Status is returned.
[[nodiscard]] Status publish_unit(CatalogStore* s, CookUnit& unit, u64 hostDigest,
                                  DiagSink const* diag) noexcept;
/// Rewrites the catalog and the input records (temporary file, then rename) if anything changed.
[[nodiscard]] Status commit_catalog(CatalogStore* s, DiagSink const* diag) noexcept;

/// The entry of `name` and `kind` in memory (views are not kept), or false.
[[nodiscard]] bool catalog_find(CatalogStore* s, AssetKind kind, StrView name, Hash128* key) noexcept;
/// Copies the input record of the unit `name` into `out`: inputs, and the outputs' names, kinds,
/// slots and current keys (no bytes). False when there is none.
[[nodiscard]] bool copy_input_record(CatalogStore* s, StrView name, CookUnit* out, u64* hostDigest) noexcept;
/// Sets the host-settings digest of the unit `name`'s record (its keys were checked again).
void set_record_digest(CatalogStore* s, StrView name, u64 hostDigest) noexcept;

/// True if the unit `d.name`'s record still describes it: the same source path, every input
/// unchanged (recorded_inputs_unchanged()), and `hostDigest` or, when the digest differs, the
/// host's settings giving the recorded keys (the record then takes `hostDigest`).
[[nodiscard]] bool record_is_current(CatalogStore* s, UnitDesc const& d, u64 hostDigest,
                                     bool rehash) noexcept;

/// Session state, never written: a unit is fresh once this process checked its inputs or cooked
/// it (publish_unit() marks it). The source poller watches the fresh units.
[[nodiscard]] bool is_fresh(CatalogStore* s, StrView name) noexcept;
void mark_fresh(CatalogStore* s, StrView name) noexcept;
/// The names of the fresh units, NUL-separated, into `out` (replaced).
void fresh_units(CatalogStore* s, Vec<char>* out) noexcept;

} // namespace kiln::cook
