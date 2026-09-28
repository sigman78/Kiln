# Demo assets

The viewer demo uses a few PBR models from the Khronos
[glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) repository. They are
not committed: `khronos/` is ignored by git and filled on demand, pinned to one commit and
verified by SHA-256, so the repository stays small.

```sh
cmake --build --preset win-msvc-debug --target viewer-assets      # any preset
# or without a build tree:
cmake -DOUT_DIR=examples/assets/khronos -P examples/viewer/fetch_assets.cmake
```

Then cook on the fly and view (the store is created next to the build):

```sh
build/win-msvc-debug/examples/viewer/kiln-viewer --source examples/assets/khronos \
    --store build/demo-store Lantern.mesh WaterBottle.mesh Avocado.mesh SheenChair.mesh BoomBox.mesh \n    DiffuseTransmissionTeacup.mesh
```

| Model | Size | License | What it shows |
|---|---|---|---|
| Lantern | 9.1 MiB | CC0-1.0 | three-part hierarchy, emissive |
| WaterBottle | 8.6 MiB | CC0-1.0 | full metal/roughness set, 2048² textures |
| Avocado | 7.7 MiB | CC0-1.0 | organic shape, normal map |
| SheenChair | 3.9 MiB | CC0-1.0 | material variants and sheen (base color only in the viewer) |
| BoomBox | 10.1 MiB | CC0-1.0 | dense mesh, emissive panel |
| DiffuseTransmissionTeacup | 4.6 MiB | CC0-1.0 | embedded JPEG base color next to PNG maps (diffuse transmission ignored in the viewer) |

All six are CC0 1.0 (public domain dedication), pinned to commit
`7d4ba189827916452eeadc82d4b712dbc6280a6f`. Models with `CC-BY-NC`, vendor test licenses
(VirtualCity, Sponza) or trademark marks were left out on purpose.

## Your own models

Drop `.glb` or `.gltf` files into this folder and point the viewer at it with
`--source examples/assets`; the asset name is the file name without extension. Git ignores model
files here, so anything with a license that does not fit the repository (for example Sketchfab
downloads under CC-BY-NC) stays on your machine.
