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
    Lantern.mesh WaterBottle.mesh Avocado.mesh SheenChair.mesh BoomBox.mesh DiffuseTransmissionTeacup.mesh

# an already cooked store, for example the test goldens
kiln-viewer --store tests/golden mesh/Box.mesh mesh/BoxTextured.mesh

# no window: render 60 frames and write the last one
kiln-viewer --offscreen --frames 60 --dump frame.png --store build/demo-store Lantern.mesh

# watch textures stream in: a small budget makes it take several frames
kiln-viewer --budget-mib 1 --store build/demo-store WaterBottle.mesh BoomBox.mesh

# your own glTF: the asset name is the file name without extension, relative to --source
kiln-viewer --source path/to/models --store build/my-store Robot.mesh

# hot reload: re-export Robot.glb and the view updates in about a second
kiln-viewer --watch --source path/to/models --store build/my-store Robot.mesh
```

`--source` enables cook-on-miss: a mesh missing from the store is cooked from `<source>/<name>.glb`
(or `.gltf`) on a worker, written to the store, and loaded. Its textures are cooked with it.

## Headless recipes

```sh
kiln-headless --store tests/golden --slow 200 --latency 5 mesh/Box.mesh ktx2/color_srgb.ktx2
kiln-headless --store build/demo-store --source examples/assets/khronos --latency 300 Lantern.mesh
# log reloads for two minutes while you edit the source
kiln-headless --watch --timeout 120 --store build/my-store --source path/to/models Robot.mesh
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
