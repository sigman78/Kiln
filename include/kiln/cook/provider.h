// kiln/cook/provider.h — cook-on-miss for dev builds: installs a CookProvider on a
// runtime Context that cooks missing assets from the context's source roots into
// its store (or only into memory in cache-less mode). kiln_cook only.
#pragma once

#include "kiln/assets.h"
#include "kiln/cook/settings.h"

namespace kiln::cook {

struct ProviderDesc {
    StoreMode storeMode = StoreMode::Disk; ///< Disk: write cooked files to store_dir(ctx); Memory: cache-less
    TargetProfile target        = {};
    MeshCookSettings mesh       = {}; ///< session overrides for every mesh
    TextureCookSettings texture = {}; ///< session overrides (usage/color space still inferred per slot)
    /// Usage of a texture with a source file of its own, when `texture.usage` is Auto. Copied
    /// at install; empty means standalone textures are cooked as Color.
    Span<NameRule const> nameRules = kDefaultNameRules;
    bool fastPreview               = false;
    /// Dev builds: poll the source files of cooked assets and re-cook them into the store when
    /// they change (docs/design/hot-reload.md). The runtime's store poller then reloads them.
    bool watchSources = false;
    u32 pollMs        = 250;
};

/// Register the provider. Source lookup: `<root>/<assetPath>.glb|.gltf` for meshes,
/// `<root>/<assetPath>.png|.jpg|.jpeg|.webp|.ktx2` for textures. A texture named
/// `<mesh>#<image>` is an embedded image: the provider cooks `<mesh>`, which writes all
/// of its embedded images. Images a mesh references by URI are not cooked with it; the
/// host requests them under names of its own. Returns InvalidArgument if the context
/// has no source roots.
/// Call install_provider and uninstall_provider on the pump thread.
KILN_API Status install_provider(Context* ctx, ProviderDesc const& desc) noexcept;
KILN_API void uninstall_provider(Context* ctx) noexcept;

} // namespace kiln::cook
