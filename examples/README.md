# Examples

## See it in one command

```sh
cmake --preset win-msvc-debug                                    # or any other preset
cmake --build --preset win-msvc-debug --target viewer-demo
```

`viewer-demo` downloads six CC0 Khronos models (about 45 MiB, once, into `examples/assets/khronos/`,
which git ignores), cooks them on the fly into `build/<preset>/demo-store`, and opens `kiln-viewer` on
them. The models appear as flat placeholders first and get their textures as the uploads land. Left-drag
orbits, the wheel zooms, Esc quits. It needs a Vulkan 1.4 driver; no SDK.

The second run is fast: the models are cached and the store is already cooked.

## The programs

| Program | Needs | What it does |
|---|---|---|
| `kiln-headless` | nothing | Loads assets through the null adapter and logs every request, event and diagnostic. `--slow` and `--latency` simulate slow IO and cooking. `--watch` keeps pumping until `--timeout` and logs hot reloads. |
| `kiln-viewer` | Vulkan 1.4 driver | Draws cooked meshes through the example Vulkan adapter (`viewer/vk_adapter.cpp`). `--watch` turns on hot reload. |
| `kiln-vk-smoke` | Vulkan 1.4 driver | Loads assets through the adapter with no window and prints what the adapter did. |

Every program prints its options with `--help`. Binaries land in `build/<preset>/examples/<name>/`.

## Viewer recipes

```sh
# the demo set by hand (what viewer-demo runs)
kiln-viewer --source examples/assets/khronos --store build/demo-store \
    Lantern.glb WaterBottle.glb Avocado.glb SheenChair.glb BoomBox.glb DiffuseTransmissionTeacup.glb

# an already cooked store, for example the test goldens (no source, so no extension in the name)
kiln-viewer --store tests/golden mesh/Box mesh/BoxTextured

# no window: render until every asset is Ready or Failed, then write that frame
kiln-viewer --offscreen --dump frame.png --store build/demo-store Lantern.glb

# no window: the frame at 50 ms after the first request, textures still streaming
kiln-viewer --offscreen --at 50 --dump streaming.png --store build/demo-store Lantern.glb

# watch textures stream in: a small budget makes it take several frames
kiln-viewer --budget-mib 1 --store build/demo-store WaterBottle.glb BoomBox.glb

# your own glTF: the asset name is the file name, extension included, relative to --source
kiln-viewer --source path/to/models --store build/my-store Robot.glb

# two source roots: the demo models as the default root, the test corpus as the root `gen`.
# The external_uri texture resolves inside `gen`; its store files go to build/roots-store/gen#/
kiln-viewer --source examples/assets/khronos --root gen=tests/corpus/gltf/generated \
    --store build/roots-store WaterBottle.glb gen:external_uri.gltf

# a sky: a cube texture behind the scene, here a vertical strip of 6 faces (+X -X +Y -Y +Z -Z)
kiln-viewer --source examples/assets/khronos --root sky=path/to/skies --sky sky:clouds_cube.png \
    --store build/demo-store WaterBottle.glb

# hot reload: re-export Robot.glb and the view updates in about a second
kiln-viewer --watch --source path/to/models --store build/my-store Robot.glb
```

`--source <dir>` sets the default root and enables cook-on-miss: a mesh missing from the store is
cooked from `<source>/<name>` on a worker, written to the store, and loaded. Its embedded textures
are cooked with it. `--root <name>=<dir>` adds a named root, whose assets are named `<name>:<path>`;
`--root <dir>` without a name is the same as `--source <dir>`. Roots have separate namespaces, and
a glTF file's references must stay inside its own root.

## Headless recipes

`kiln-headless` pumps until every asset is Ready or Failed (or `--timeout`), resting 1 ms after a
pump with nothing to do. Log lines carry milliseconds since start.

```sh
kiln-headless --store tests/golden --slow 200 --latency 5 mesh/Box.mesh ktx2/color_srgb.ktx2
kiln-headless --store build/demo-store --source examples/assets/khronos --latency 300 Lantern.glb.mesh
# two roots, with committed files only (the example_headless_roots test runs this)
kiln-headless --store build/roots-store --source tests/corpus/gltf/khronos \
    --root gen=tests/corpus/gltf/generated Box.glb.mesh gen:external_uri.gltf.mesh \
    "gen:pbr_textures.glb#hull_albedo.ktx2"
# log reloads for two minutes while you edit the source
kiln-headless --watch --timeout 120 --store build/my-store --source path/to/models Robot.glb.mesh
```

## Adapter smoke

```sh
kiln-vk-smoke --store tests/golden --staging-kib 4 --frames 50 mesh/Box.mesh ktx2/normal.ktx2
```

A 4 KiB staging ring forces `Busy` back-pressure; the stats line at the end shows the retries and that
every asset still reached Ready.

## Where things are

- `headless/main.cpp`: the runtime API walkthrough, numbered step by step.
- `viewer/vk_adapter.cpp`: the example adapter. `viewer/main.cpp` and `viewer/viewer_render.cpp`: the
  viewer. `viewer/shaders/`: GLSL plus the committed SPIR-V.
- `assets/README.md`: the demo models, their licenses and the pinned commit.
- Design: `docs/design/viewer.md`.
