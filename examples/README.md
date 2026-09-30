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
| `kiln-gl` | OpenGL 4.6 driver | One model and a cube sky through the example GL adapter (`gl/gl_adapter.cpp`): textures bound per draw, uploads flushed by the host on the GL thread. The integration example for GL users (`docs/design/integration-examples.md`). |
| `kiln-gl-bindless` | OpenGL 4.6 + `ARB_bindless_texture` | The same scene with bindless textures: each texture gets a slot in a table of resident handles at request time, and materials store slot numbers once. Renders the same pixels as `kiln-gl`. |
| `kiln-gl-array` | OpenGL 4.6 driver | A texture array that kiln assembles at load time from six separately cooked PNG tiles (`request_texture_array`, `docs/design/runtime-texture-arrays.md`), drawn as a floor of tiles through one `sampler2DArray`; each tile shows its layer number. The adapter gets one upload with every layer. Edit a tile under `examples/assets/tiles/` and the array reloads. Built with `KILN_EXAMPLE_GL`; needs no downloaded model. |
| `kiln-vk-array` | Vulkan 1.4 driver | The same floor of tiles through Vulkan, in both binding models: by default a descriptor set per frame in flight, rewritten when kiln's events say the array's GPU object changed (as `kiln-vk-basic`); with `--bindless`, the array's kiln slot in the adapter's `sampler2DArray` binding (as the viewer). Both draw the same pixels. The floor is the frame plumbing's full-screen pass with this example's shaders (`vk-array/shaders/`). Built with `KILN_EXAMPLE_VK_BASIC`; needs no downloaded model. |
| `kiln-sokol` | D3D11 (Windows), GL 4.3 (Linux) or Metal (macOS) | The same scene through sokol_gfx, with sokol_app owning the main loop: kiln's `create()`, `pump()` and `destroy()` live in the app callbacks. `--offscreen` shows the window until the scene settles (sokol_app has no hidden windows). |
| `kiln-vk-basic` | Vulkan 1.4 driver | The same scene on Vulkan without bindless: a descriptor set per material and frame slot, rewritten when kiln's events say a texture's object changed. Shares the viewer's device, adapter (bindless off) and frame plumbing. |
| `kiln-nga` | Vulkan 1.4 with `VK_EXT_descriptor_heap` (RTX 30+, RDNA 3+) | The same scene through NoGraphicsAPI: vertices pulled through GPU pointers from the payload kiln wrote in place, textures as descriptor heap indices. Only with `-DKILN_EXAMPLE_NGA=ON` (fetches NoGraphicsAPI, builds the Vulkan loader, downloads Slang). Headless on Linux (`--offscreen`). |
| `kiln-vk-smoke` | Vulkan 1.4 driver | Checks that uploads complete in commit order, then loads assets through the adapter with no window and prints what the adapter did. |

Every program prints its options with `--help`. Binaries land in `build/<preset>/examples/<name>/`.

## Planned example: `kiln-gl-minimal` (not implemented)

A small introductory OpenGL adapter with serial loading on the GL context thread: an inline
`JobSystem`, CPU scratch per upload, and ordinary GL texture/buffer uploads in `commit_upload`.
The example would load a pre-cooked mesh and 2D textures before rendering, with no adapter queues,
mutexes, staging rings, or bindless bookkeeping. Loading blocks the calling thread; kiln still
advances asset states through `pump()` / `wait()`.

This is a design proposal only: there is no source directory, executable, or CMake option yet.
See [the proposed adapter and scope](../docs/design/integration-examples.md#planned-minimal-synchronous-gl-example).

## Viewer recipes

```sh
# the demo set by hand (what viewer-demo runs)
kiln-viewer --source examples/assets/khronos --store build/demo-store \
    Lantern.glb WaterBottle.glb Avocado.glb SheenChair.glb BoomBox.glb DiffuseTransmissionTeacup.glb

# an already cooked store, for example the test goldens as a store (ctest's golden_store fixture
# writes it; no source, so no extension in the name)
kiln-viewer --store build/win-msvc-debug/tests/samples/golden-store mesh/Box mesh/BoxTextured

# no window: render until every asset is Ready or Failed, then write that frame
kiln-viewer --offscreen --dump frame.png --store build/demo-store Lantern.glb

# no window: the frame at 50 ms after the first request, textures still streaming
kiln-viewer --offscreen --at 50 --dump streaming.png --store build/demo-store Lantern.glb

# watch textures stream in: a small budget makes it take several frames
kiln-viewer --budget-mib 1 --store build/demo-store WaterBottle.glb BoomBox.glb

# your own glTF: the asset name is the file name, extension included, relative to --source
kiln-viewer --source path/to/models --store build/my-store Robot.glb

# two source roots: the demo models as the default root, the test corpus as the root `gen`.
# The external_uri texture resolves inside `gen`, and is named gen:external_uri_albedo.png
kiln-viewer --source examples/assets/khronos --root gen=tests/corpus/gltf/generated \
    --store build/roots-store WaterBottle.glb gen:external_uri.gltf

# a sky: a cube texture behind the scene, a vertical strip of 6 faces (+X -X +Y -Y +Z -Z);
# the committed test skies are in examples/assets/skies (see its README)
kiln-viewer --source examples/assets/khronos --root sky=examples/assets/skies --sky sky:test_cube.png \
    --store build/demo-store WaterBottle.glb

# an HDR sky (a Radiance .hdr strip): tonemapped with ACES automatically; -2 EV darkens it
kiln-viewer --source examples/assets/khronos --root sky=examples/assets/skies --sky sky:hdr_cube.hdr \
    --exposure -2 --store build/demo-store WaterBottle.glb

# hot reload: re-export Robot.glb and the view updates in about a second
kiln-viewer --watch --source path/to/models --store build/my-store Robot.glb
```

`--source <dir>` sets the default root and enables cook-on-miss: a mesh missing from the store is
cooked from `<source>/<name>` on a worker, written to the store, and loaded. Its embedded textures
are cooked with it. `--root <name>=<dir>` adds a named root, whose assets are named `<name>:<path>`;
`--root <dir>` without a name is the same as `--source <dir>`. Roots have separate namespaces, and
a glTF file's references must stay inside its own root.

## Integration examples: nothing to pass

`kiln-gl`, `kiln-gl-bindless`, `kiln-sokol`, `kiln-vk-basic` and `kiln-nga` all show the same
reference scene, WaterBottle under the HDR test sky, and take no arguments:

```sh
cmake --build --preset win-msvc-debug --target viewer-assets   # once: downloads the demo models
build/win-msvc-debug/examples/gl/kiln-gl                       # or any of the others
```

On first run the model and the textures cook into the build tree's `example-store`, so the first
frames show placeholders. All the examples cook with the default target profile, `compat`, whose
block formats every example backend samples, so they share that one store
(docs/design/target-profiles.md). A store from before the flat layout of `store-manifest.md` (named
files and `kiln-store.txt`, or `catalogs/` and `artifacts/`) is not read; delete it once. Left-drag orbits, the wheel zooms, + and - change the exposure, Esc
quits. Hot reload is on: re-export or edit a file under `examples/assets` and the view updates.
`--dump <file.png>` waits until everything has loaded, writes the frame and exits (the window is
hidden where the API allows; sokol_app always shows one). Meshes cook as plain floats
(`VertexProfile::Float`), so the shaders need no vertex decoding.

## Headless recipes

`kiln-headless` pumps until every asset is Ready or Failed (or `--timeout`), resting 1 ms after a
pump with nothing to do. Log lines carry milliseconds since start.

```sh
kiln-headless --store build/win-msvc-debug/tests/samples/golden-store --slow 200 --latency 5 mesh/Box.mesh ktx2/color_srgb.ktx2
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
- `gl/main.cpp` and `gl/main_bindless.cpp`: the GL integration examples, numbered step by step (compare
  the two to see what bindless changes); `gl/gl_adapter.cpp`: their adapter, bound or bindless;
  `gl/gl_util.cpp`: what the two share; `gl/gl_api.cpp`: a GL loader of about 75 functions over GLFW.
- `nga/main.cpp`, `nga/nga_adapter.cpp`, `nga/scene.slang`: the NoGraphicsAPI example.
- `vk-basic/main.cpp`: the non-bindless Vulkan example (compare `viewer/main.cpp` for bindless).
- `sokol/main.cpp`: the sokol integration example; `sokol/sokol_adapter.cpp`: its adapter;
  `sokol/scene.glsl`: its shaders, compiled by sokol-shdc at build time.
- What each integration example showed about kiln's API: `docs/api-friction.md`. `common/`: window, camera and logging
  shared by the windowed examples.
- `viewer/vk_adapter.cpp`: the example adapter. `viewer/main.cpp` and `viewer/viewer_render.cpp`: the
  viewer. `viewer/shaders/`: GLSL plus the committed SPIR-V.
- `assets/README.md`: the demo models, their licenses and the pinned commit.
- Design: `docs/design/viewer.md`.
