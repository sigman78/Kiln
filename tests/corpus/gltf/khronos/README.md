# Khronos glTF-Sample-Assets test files

Unmodified copies of files from the
[glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) repository, used
as real-world reader input for the M2 cooker's glTF import tests. Expected values live in
`../manifest.txt`.

- Source: `https://github.com/KhronosGroup/glTF-Sample-Assets`, commit
  `7d4ba189827916452eeadc82d4b712dbc6280a6f` (pinned via `gh api
  repos/KhronosGroup/glTF-Sample-Assets/commits/main --jq .sha` on 2026-09-27), directory
  `Models/<Name>/glTF-Binary/`. Not stored in Git LFS; downloaded directly from
  `https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/7d4ba189827916452eeadc82d4b712dbc6280a6f/Models/<Name>/glTF-Binary/<Name>.glb`.
- Licenses: per-model `metadata.json` "legal" section (fetched with `gh api
  repos/KhronosGroup/glTF-Sample-Assets/contents/Models/<Name>/metadata.json`), reproduced
  below. Two files are CC-BY-4.0 (attribution required) and two are CC0-1.0 (no
  attribution required); the CC-BY-4.0 license text is included as `LICENSE.CC-BY-4.0`
  (fetched from `https://creativecommons.org/licenses/by/4.0/legalcode.txt`).
- Ground truth for the manifest: read directly from each file's JSON chunk (no
  `gltf-transform`/`gltf-validator` available on this machine — `where gltf-validator`
  found nothing); a small Python script (`struct` + `json`, no dependencies) parsed the
  GLB header and dumped `nodes`/`meshes`/`materials`/`images`/`textures`. Uncertain fields
  are marked `?` in `../manifest.txt` and explained in the notes column there.

Four models were picked (not five — `BoxTextured`, `MultiUVTest` and `BoxVertexColors`
were also checked and are all suitably small/permissively licensed, but the task asked for
3-4): a plain box, a textured box, a box with no material at all plus vertex colors, and a
box with a second UV set feeding a different texture slot. All are well under the ~600 KB
per-file / ~1.5 MB total budget.

| File | Bytes | Source path | License | Author / attribution | Covers |
|---|---:|---|---|---|---|
| `Box.glb` | 1664 | `Models/Box/glTF-Binary/Box.glb` | CC-BY-4.0 | Cesium, 2017 | Baseline: 1 mesh, 1 material (factor-only), a non-identity root transform (coordinate-fixup rotation matrix) above the mesh node. |
| `BoxTextured.glb` | 5956 | `Models/BoxTextured/glTF-Binary/BoxTextured.glb` | CC-BY-4.0 (+ non-copyrightable Cesium logo trademark notice, `LicenseRef-LegalMark-Cesium`, for the logo baked into the texture) | Cesium, 2017 | A real (non-procedural) `baseColorTexture` + sampler, same root-transform shape as `Box.glb`. |
| `BoxVertexColors.glb` | 1924 | `Models/BoxVertexColors/glTF-Binary/BoxVertexColors.glb` | CC0-1.0 (no attribution required) | Marco Hutter, 2023 | `COLOR_0` vertex attribute (`MaterialSlot.flags` bit 0, "uses vertex color"); the primitive has **no material at all** (`materials` is absent from the document), exercising the cooker's default-material fallback on a real-world file, not just a hand-made one. |
| `MultiUVTest.glb` | 43004 | `Models/MultiUVTest/glTF-Binary/MultiUVTest.glb` | CC-BY-4.0 (+ non-copyrightable Khronos logo trademark notice, `LicenseRef-LegalMark-Khronos`, for the logo baked into one texture) | Hilo 3D, 2017 | Real second UV set: `TEXCOORD_0` feeds `baseColorTexture`, `TEXCOORD_1` feeds `emissiveTexture` (`texCoord: 1`). Also has `TANGENT`, and two unnamed camera nodes as scene-root siblings of the mesh node — nodes with no mesh, no mount/col/`_` prefix, and no kiln equivalent (cameras aren't part of `.mesh`); see the `?` note in `../manifest.txt`. |

`OrientationTest.glb` (node rotation/matrix stress test) and `SimpleMeshes` were also
considered; `OrientationTest`'s negative-scale/rotation coverage overlaps with the
hand-made `hierarchy_parts.glb` (which was written specifically to exercise that), and
`SimpleMeshes` ships no `glTF-Binary` variant, so both were left out to keep the set at 4.
