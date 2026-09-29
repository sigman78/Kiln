// mesh_cook_off.cpp — cook_mesh in a KILN_MESH=OFF build: no importer, no mesh cooker.
// docs/design/texture-only.md.
#include "kiln/cook/cook.h"

namespace kiln::cook {

Result<CookedMesh> cook_mesh(MeshSource const& src, MeshCookSettings const&, TargetProfile const&,
                             CookEnv const& env) noexcept {
    return diagf(env.diag, make_status(Code::Unsupported), kDiagMeshCookNotBuilt, Severity::Error,
                 src.assetPath, "cook_mesh", "mesh cooking is not built into this kiln (KILN_MESH=OFF)");
}

} // namespace kiln::cook
