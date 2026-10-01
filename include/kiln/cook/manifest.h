// kiln/cook/manifest.h — build keys of cooked artifacts and the manifest writer
// (docs/design/store-manifest.md). kiln_cook only.
#pragma once

#include "kiln/containers.h"
#include "kiln/manifest.h"

namespace kiln::cook {

/// Bump when the serialization in build_key() changes.
inline constexpr u32 kBuildKeySchema = 1;

/// What an input file is to the cook that read it.
enum class InputRole : u8 {
    Source  = 1, ///< the source file of the asset (or of its owner, for an embedded image)
    Sidecar = 2, ///< the source's `.kiln` file
    Buffer  = 3, ///< an external file a `.gltf` references by URI
};

/// One input file of a cook. `name` identifies it without its directory: the source's asset
/// name for Source and Sidecar, the URI as written for Buffer.
struct BuildInput {
    InputRole role = InputRole::Source;
    StrView name   = {};
    Hash128 content; ///< xxh3_128 of the file's bytes
};

struct BuildKeyDesc {
    AssetKind kind   = AssetKind::Mesh;
    StrView name     = {}; ///< the asset's canonical name; `<mesh>#<image>` for an embedded image
    u64 targetHash   = 0;  ///< hash_target()
    u64 settingsHash = 0;  ///< hash_settings() of the resolved settings
    Span<BuildInput const> inputs = {}; ///< in the order the cook read them
};

/// XXH3-128 over a field-by-field serialization of `d`, kBuildKeySchema and kCookerVersion.
/// The same key means the same cooked bytes.
KILN_API Hash128 build_key(BuildKeyDesc const& d);

/// Diagnostics of store writers (K3009, K3010; the K3000 range of settings.h).
enum StoreDiagCode : u32 {
    kDiagStoreLocked = 3009, ///< another process writes the store (Busy)
    kDiagNondeterministicCook =
        3010, ///< a cook made other bytes for an existing build key (ValidationFailed)
};

struct ManifestProfileDesc {
    StrView name                      = {};
    u64 hash                          = 0;  ///< hash_target
    u64 blockFormats                  = 0;  ///< block_format_bit() set
    Span<ManifestEntry const> entries = {}; ///< any order
};

struct ManifestDesc {
    Span<ManifestProfileDesc const> profiles = {}; ///< any order
};

/// `<store>/manifest.in`: the input records of every profile, cook only (never shipped). Its format
/// is the cook's own (docs/design/store-manifest.md); a damaged one only costs time.
inline constexpr char kInputRecordsFile[] = "manifest.in";
/// `<store>/manifest.lock`: held by the one process that writes the store.
inline constexpr char kStoreLockFile[] = "manifest.lock";

/// Writes a manifest (format 0.1) into `out`, replacing its contents. The same profiles and entries
/// in any order give the same bytes. InvalidArgument for a bad profile or asset name (K4205), or a
/// profile name, or a name and kind in one profile, given twice (K4207).
KILN_API Status write_manifest(ManifestDesc const& d, Vec<u8>* out, DiagSink const* diag = nullptr);

} // namespace kiln::cook
