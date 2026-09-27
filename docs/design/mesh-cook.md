# Mesh cooker pipeline

**Status:** Proposed (awaiting owner sign-off). Implemented in M2: `src/cook/gltf_import.cpp`,
`src/cook/mesh_cook.cpp`.
**Decides:** The stage order of the glTF to `.mesh` cooker and the determinism rules it follows.
The file format is in `../mesh-format-spec.md`; task splitting is in `cook-kernels.md`.

## Stages

```
import (gltf_import.cpp) -> per (part, LOD): expand and bake transforms, one submesh per material
-> generated normals -> MikkTSpace tangents -> weld -> optimize (meshoptimizer)
-> quantize (mesh-format-spec §6) -> LOD / submesh / material records -> write
```

- Normals are generated only where a primitive has none. Tangents run on an unindexed triangle
  list, so welding follows them.
- Building (expand to optimize) and quantizing run per (part, LOD) task; records and interning run
  sequentially in traversal order (`cook-kernels.md`).

### Import

- Parses GLB or `.gltf` JSON with cgltf. cgltf's memory hooks go to the cook arena.
- Buffers: the GLB BIN chunk and `data:` URIs go through cgltf. External URIs go through
  `MeshSource::resolver`, never the file system directly.
- Rejects Draco and `EXT_meshopt_compression` (K1002) and sparse accessors (K1003, see
  `../diagnostics.md`). Every accessor read goes through one buffer-view resolver, the future
  `EXT_meshopt_compression` decode hook (`dependencies.md`).
- Traverses the scene with the naming conventions (`col_` and `_` skip a subtree, `mount_`,
  `_lodN`) and reduces node transforms to a part translation and rotation plus a residual matrix
  baked into the vertices.
- Output is `detail::ImportScene`, with all memory in the cook arena. Every attribute is expanded
  to `f32`.

## Determinism

- Iteration is always in file or traversal order, never over hash maps or pointer values.
- Float math is limited to products, sums, divisions, `std::sqrt` and `std::floor`.
- Half floats are converted by bit manipulation.
- The working vertex struct has no padding, so it can be hashed and compared bytewise.
