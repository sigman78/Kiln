// kiln/cook/provider.h — the cook provider for dev builds: keeps a Context's store up to
// date from the context's roots (or cooks only into memory, cache-less). kiln_cook only.
#pragma once

#include "kiln/assets.h"
#include "kiln/cook/settings.h"

namespace kiln::cook {

struct ProviderDesc {
    StoreMode storeMode  = StoreMode::Disk; ///< Disk: publish into the store; Memory: cache-less
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
    /// Caps the encoder effort of the textures this provider cooks, for fast iteration. An entry
    /// cooked at a higher quality (by kiln-cook) stays in use until its sources change.
    EncodeQuality maxQuality = EncodeQuality::Fast;
    /// Manifest layout: the version of the host's `policy`. A policy is code, so the provider cannot
    /// see it change: bump this when it does, and every entry's key is checked again.
    u32 policyVersion = 0;
    /// Dev builds: poll the source files of what this provider cooked, and of requests whose cook
    /// failed, and reload their assets when they change (post_reload(); docs/design/hot-reload.md).
    /// Disk mode cooks them into the store first; Memory mode cooks them on the reload. The context
    /// needs no store poller for this.
    bool watchSources = false;
    u32 pollMs        = 250;
    /// The project file (kiln.toml, docs/design/project-config.md): settings layers 3a to 3c between
    /// the defaults above and sidecars. Its [roots] and [project] tables are not used here. An error
    /// in it fails install_provider. With `watchSources`, an edit is loaded again and the assets it
    /// changes cook again; an edit with errors keeps the previous project. Empty: none.
    StrView projectFile = {};
};

/// Register the provider. The source of `root:path` is `<dir of root>/path`; the extension
/// gives the kind: `.glb` `.gltf` a mesh; `.png` `.jpg` `.jpeg` `.webp` `.hdr` `.ktx2` a texture.
/// A texture named `<mesh>#<image>` is an embedded image: the provider cooks `<mesh>`, which
/// writes all of its embedded images. Images a mesh references by URI are not cooked with
/// it; the host requests them by name. Returns InvalidArgument if the context has no roots.
/// The context's profile must be `desc.target`'s (K3008). Each asset is checked once per session
/// by the size and time of its recorded inputs (a new time is hashed first), and cooked again when
/// their content changed. In Disk mode the provider writes the profile's entries in the store's
/// manifest and holds the store's lock until it is released (K3009 for a second writer); in Memory
/// mode it cooks every load and writes nothing.
/// Call install_provider and uninstall_provider on the pump thread. destroy(ctx) frees a provider
/// that is still installed, so uninstall_provider is needed only to remove it earlier.
KILN_API Status install_provider(Context* ctx, ProviderDesc const& desc);
KILN_API void uninstall_provider(Context* ctx);

/// True if every segment of `path` (`/`-separated, relative to `root`) has the same case on
/// disk. Case-insensitive file systems accept a name in the wrong case, which then fails
/// elsewhere. Checked on Windows; true on other systems.
[[nodiscard]] KILN_API bool source_case_matches(StrView root, StrView path);

enum ProviderDiagCode : u32 {
    kDiagSourceKind  = 5014, ///< the name's extension does not give the requested kind
    kDiagUnknownRoot = 5015, ///< the name's root is not one of the context's roots
    kDiagSourceCase  = 5016, ///< the name and the source file differ in case
};

} // namespace kiln::cook
