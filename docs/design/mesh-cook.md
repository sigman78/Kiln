# Mesh cooker pipeline

Status: **Proposed** (2026-09-27). Records the stage order of the glTF to `.mesh` cooker and
the determinism rules it follows. The file format itself is in `../mesh-format-spec.md`.

## Stages

```
import (gltf_import.cpp) -> scale baking -> split per material -> welding ->
generated normals -> MikkTSpace tangents -> optimize (meshoptimizer) ->
quantize (mesh-format-spec §6) -> LOD / submesh / material records -> write
```

### Import

- Parses GLB or `.gltf` JSON with cgltf. cgltf's memory hooks route to the cook arena.
- Buffers: the GLB BIN chunk and `data:` URIs go through cgltf. External URIs go through
  `MeshSource::resolver`, never the file system directly.
- Rejects Draco, `EXT_meshopt_compression` and sparse accessors (K1002, see
  `../diagnostics.md`). Every accessor read goes through one buffer-view resolver, which is the
  future `EXT_meshopt_compression` decode hook (`dependencies.md`). That decode would use
  `meshopt_decodeVertexBuffer` / `meshopt_decodeIndexBuffer` into the arena.
- Traverses the scene with the naming conventions (`col_`, `_`, `mount_`, `_lodN`) and reduces
  node transforms to a part translation and rotation plus a residual matrix baked into the
  vertices.
- Output is `detail::ImportScene`, with all memory in the cook arena. Every attribute is
  expanded to `f32`.

## Determinism

- Iteration is always in file or traversal order, never over hash maps or pointer values.
- Float math is limited to products, sums, divisions, `std::sqrt` and `std::floor`.
- Half floats are converted by bit manipulation.
- The working vertex struct has no padding, so it can be hashed and compared bytewise.
