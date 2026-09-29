// kiln/cook/provider.h — cook-on-miss for dev builds: installs a CookProvider on a
// runtime Context that cooks missing assets from the context's roots into its store
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
    /// Catalog layout: the version of the host's `policy`. A policy is code, so the provider cannot
    /// see it change: bump this when it does, and every entry's key is checked again.
    u32 policyVersion = 0;
    /// Dev builds: poll the source files of cooked assets and re-cook them into the store when
    /// they change (docs/design/hot-reload.md). The runtime's store poller then reloads them.
    bool watchSources = false;
    u32 pollMs        = 250;
};

/// Register the provider. The source of `root:path` is `<dir of root>/path`; the extension
/// gives the kind: `.glb` `.gltf` a mesh; `.png` `.jpg` `.jpeg` `.webp` `.hdr` `.ktx2` a texture.
/// A texture named `<mesh>#<image>` is an embedded image: the provider cooks `<mesh>`, which
/// writes all of its embedded images. Images a mesh references by URI are not cooked with
/// it; the host requests them by name. Returns InvalidArgument if the context has no roots.
/// With StoreMode::Disk the store must be empty or cooked for `desc.target`'s profile
/// (bind_store_profile); otherwise it returns InvalidArgument (K3008) and writes nothing.
/// In the Catalog layout the context's profile must be `desc.target`'s (K3008). The provider then
/// installs CookProvider::prepare: each asset is checked once per session by the size and time of
/// its recorded inputs, and cooked again when they changed. In Disk mode it writes the profile's
/// catalog and holds its lock until it is released (K3009 for a second writer).
/// Call install_provider and uninstall_provider on the pump thread. destroy(ctx) frees a provider
/// that is still installed, so uninstall_provider is needed only to remove it earlier.
KILN_API Status install_provider(Context* ctx, ProviderDesc const& desc) noexcept;
KILN_API void uninstall_provider(Context* ctx) noexcept;

/// True if every segment of `path` (`/`-separated, relative to `root`) has the same case on
/// disk. Case-insensitive file systems accept a name in the wrong case, which then fails
/// elsewhere. Checked on Windows; true on other systems.
[[nodiscard]] KILN_API bool source_case_matches(StrView root, StrView path) noexcept;

enum ProviderDiagCode : u32 {
    kDiagSourceKind  = 5014, ///< the name's extension does not give the requested kind
    kDiagUnknownRoot = 5015, ///< the name's root is not one of the context's roots
    kDiagSourceCase  = 5016, ///< the name and the source file differ in case
};

} // namespace kiln::cook
