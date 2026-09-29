// kiln/cook/catalog.h — build keys of cooked artifacts (docs/design/store-catalog.md). kiln_cook only.
#pragma once

#include "kiln/assets.h"
#include "kiln/catalog.h"

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
[[nodiscard]] KILN_API Hash128 build_key(BuildKeyDesc const& d) noexcept;

} // namespace kiln::cook
