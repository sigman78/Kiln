// kiln/cook/provider.h — cook-on-miss for dev builds: installs a CookProvider on a
// runtime Context that cooks missing assets from the context's mounts into its store
// (or only into memory in cache-less mode). kiln_cook only.
#pragma once

#include "kiln/assets.h"
#include "kiln/cook/settings.h"

namespace kiln::cook {

struct ProviderDesc {
    StoreMode storeMode = StoreMode::Disk; ///< Disk: write cooked files to store_dir(ctx); Memory: cache-less
    TargetProfile target = {};
    /// The host's settings (resolution layer 2): the base that sidecars, inference and the
    /// policy build on (docs/design/settings.md, "Resolution layers").
    MeshCookSettings meshDefaults       = {};
    TextureCookSettings textureDefaults = {};
    /// The host's last word on every asset (layer 6). Its `user` must outlive the provider.
    CookPolicy policy = {};
    /// Usage of a texture with a source file of its own when no earlier layer set it (layer 5).
    /// Copied at install; empty means such textures are cooked as Color.
    Span<NameRule const> nameRules = kDefaultNameRules;
    bool fastPreview               = false;
    /// Dev builds: poll the source files of cooked assets and re-cook them into the store when
    /// they change (docs/design/hot-reload.md). The runtime's store poller then reloads them.
    bool watchSources = false;
    u32 pollMs        = 250;
};

/// Register the provider. The source of `mount:path` is `<root of mount>/path`; the extension
/// gives the kind: `.glb` `.gltf` a mesh; `.png` `.jpg` `.jpeg` `.webp` `.ktx2` a texture.
/// A texture named `<mesh>#<image>` is an embedded image: the provider cooks `<mesh>`, which
/// writes all of its embedded images. Images a mesh references by URI are not cooked with
/// it; the host requests them by name. Returns InvalidArgument if the context has no mounts.
/// Call install_provider and uninstall_provider on the pump thread.
KILN_API Status install_provider(Context* ctx, ProviderDesc const& desc) noexcept;
KILN_API void uninstall_provider(Context* ctx) noexcept;

/// True if every segment of `path` (`/`-separated, relative to `root`) has the same case on
/// disk. Case-insensitive file systems accept a name in the wrong case, which then fails
/// elsewhere. Checked on Windows; true on other systems.
[[nodiscard]] KILN_API bool source_case_matches(StrView root, StrView path) noexcept;

enum ProviderDiagCode : u32 {
    kDiagSourceKind   = 5014, ///< the name's extension does not give the requested kind
    kDiagUnknownMount = 5015, ///< the name's mount is not one of the context's mounts
    kDiagSourceCase   = 5016, ///< the name and the source file differ in case
};

} // namespace kiln::cook
