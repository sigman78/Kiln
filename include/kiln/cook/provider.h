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
    bool fastPreview            = false;
};

/// Register the provider. Source lookup: `<root>/<assetPath>.glb|.gltf` for meshes,
/// `<root>/<assetPath>.png|.ktx2` for textures; a texture with no source file of its
/// own is produced by cooking its owning mesh (the parent path) which emits its
/// embedded textures. Returns InvalidArgument if the context has no source roots.
/// Call install_provider and uninstall_provider on the pump thread.
KILN_API Status install_provider(Context* ctx, ProviderDesc const& desc) noexcept;
KILN_API void uninstall_provider(Context* ctx) noexcept;

} // namespace kiln::cook
