# Architecture

**Decides:** nothing. This note draws what the other design notes decide: the building blocks and
their dependencies, one asset load, and one hot reload. When a diagram and a note disagree, the
note wins; fix the diagram.

## Building blocks

Thick arrows carry asset data from the sources to the GPU. Thin arrows are link dependencies.
Green ships in a release build, orange is dev builds and tools only, blue is the host's code, grey
is files on disk.

```mermaid
flowchart TB
    cli["kiln-cook<br/>batch cook CLI"]
    sources[("Source files<br/>glb · gltf · png · jpg · ktx2<br/>.kiln sidecars")]
    app["Host application<br/>engine · editor<br/>kiln-viewer · kiln-headless<br/><i>+ kiln_cook in dev builds</i>"]

    cook["<b>kiln_cook</b><br/>glTF → .mesh<br/>images → KTX2<br/>settings · sidecars<br/>cook-on-miss provider<br/>source watch"]
    third["cgltf · MikkTSpace · meshoptimizer<br/>wuffs · bc7enc_rdo · ispc_bc6h<br/>zstd encoder"]
    zstd["zstd decoder"]

    store[("Store<br/>name.mesh<br/>name.ktx2")]

    runtime["<b>kiln_runtime</b><br/>requests · pump · events<br/>loader jobs · readers<br/>IO backend<br/>store watch"]

    core["<b>kiln_core</b><br/>allocators · containers<br/>Status · diagnostics · log"]
    adapter["<b>Adapter</b><br/>host implements<br/>GPU upload · bind · destroy"]

    cli --> cook
    sources ==>|read| cook
    app --> runtime
    app ~~~ cook
    cook --> third
    cook ==>|write| store
    store ==>|read| runtime
    cook -->|"CookProvider<br/>on store miss"| runtime
    runtime --> core
    runtime --> zstd
    runtime ==>|"upload · bind · destroy"| adapter

    classDef ship fill:#e3f2e6,stroke:#2e7d32,color:#1b3a1f
    classDef dev fill:#fff3e0,stroke:#e08a00,color:#4a2c00
    classDef host fill:#e8eaf6,stroke:#3949ab,color:#1a1f4d
    classDef data fill:#f5f5f5,stroke:#757575,color:#212121
    class runtime,core,zstd ship
    class cook,third,cli dev
    class app,adapter host
    class sources,store data
```

The rules behind the picture:

- `kiln_core` depends on nothing else in kiln. `kiln_runtime` never depends on `kiln_cook`; the
  shipping build has no `kiln_cook` and no third-party code (`shipping-split.md`).
- The runtime reaches the GPU only through the host's `Adapter` (`adapter.md`). The null adapter
  serves tests and `kiln-headless`.
- The cook side joins the runtime through one function pointer, `CookProvider`. The runtime
  asks it for bytes on a store miss and knows nothing else about sources.
- One source file gives one cooked asset (`asset-model-next.md`). The store is the only thing the
  runtime reads.

## Asset load

`request_mesh` or `request_texture` on the pump thread, then `pump()` until the asset is `Ready`.
Workers run one stage of one asset per job; only `pump()` changes state and emits events
(`threading-and-io.md`, `handles-and-states.md`).

```mermaid
sequenceDiagram
    autonumber
    participant Host as Host (pump thread)
    participant Ctx as Context (registry)
    participant Worker as Worker job
    participant Prov as Cook provider (dev)
    participant Store as Store on disk
    participant Adapter as Adapter (host GPU)

    Host->>Ctx: request_mesh("props/chair.glb")
    Ctx->>Ctx: check_asset_name, AssetId = FNV-1a 64 of the name
    Ctx-->>Host: handle (state Pending)

    Host->>Ctx: pump()
    Ctx->>Worker: dispatch meta stage (High before Normal, up to maxIoJobs)
    Worker->>Store: open props/chair.glb.mesh
    opt store miss and a provider is installed
        Worker->>Prov: cook(Mesh, name)
        Prov->>Prov: find the source in its root, check the kind and case
        Prov->>Store: write the .mesh and its embedded .ktx2 files
        Prov-->>Worker: cooked bytes
    end
    Note over Worker,Store: no store file and no cooked bytes: completion Failed (K5001), the load stops
    Worker->>Worker: validate the header, read the metadata
    Worker-->>Ctx: completion MetaReady (queued under a mutex)

    Host->>Ctx: pump()
    Ctx-->>Host: event MetaReady (mesh_view and texture_info readable)
    Ctx->>Worker: dispatch upload stage (within the per-pump byte budget)
    Worker->>Adapter: begin_upload(desc) gives a destination and a token
    Worker->>Store: read the payload into the destination
    Worker->>Adapter: commit_upload(token)
    Worker-->>Ctx: completion Uploaded

    loop every pump() while upload_status is Pending
        Host->>Ctx: pump()
        Ctx->>Adapter: upload_status(token)
    end
    alt Complete
        Ctx-->>Host: event Ready (gpu_object(handle) is the real object)
    else Failed (the adapter could not make the object)
        Ctx->>Adapter: destroy(object)
        Ctx-->>Host: event Failed (K5004)
    end
```

A failure at any stage ends in one `Failed` event with one K5xxx diagnostic, emitted by `pump()`.
A texture serves its placeholder until `Ready`, and after `Failed`.

## Hot reload

Two pollers that do not know each other, joined by the store on disk (`hot-reload.md`). The
source poller exists only with a cook provider and `ProviderDesc::watchSources`; the store poller
only with `KILN_HOT_RELOAD` and `ContextDesc::hotReload.watchStore`. Either one also works alone.

```mermaid
sequenceDiagram
    autonumber
    actor Artist
    participant Src as Source file
    participant SP as Source poller (kiln_cook)
    participant Store as Store on disk
    participant RP as Store poller (kiln_runtime)
    participant Host as Host (pump thread)
    participant Worker as Worker job
    participant Adapter as Adapter (host GPU)

    Artist->>Src: save chair.glb (or its .kiln sidecar)
    loop every pollMs
        SP->>Src: stat source and sidecar
    end
    SP->>SP: size or mtime changed, re-cook
    SP->>Store: overwrite chair.glb.mesh and its embedded textures
    Note over SP,Store: a failed re-cook logs an error and keeps the old store files

    loop every pollMs
        RP->>Store: stat the files of loaded assets
    end
    RP->>Host: changed slot index (mutex list)
    Host->>Host: pump() calls request_reload(handle)
    Note over Host: the asset stays Ready, gpu_object() still returns the old object

    Host->>Worker: meta stage into the next metadata set
    Worker->>Store: read the new metadata
    Worker-->>Host: completion
    Host->>Worker: upload stage
    Worker->>Adapter: begin_upload into a new object
    Worker->>Store: read the payload
    Worker->>Adapter: commit_upload(token)
    Worker-->>Host: completion

    alt both stages succeed
        Host->>Adapter: upload_status(token)
        Host->>Adapter: bind(slot, new object), bindless textures only
        Note over Host,Adapter: the old object goes to destroy() once the host's frames that used it complete
        Host->>Host: event Changed (Ready, if the asset was Failed)
    else a stage fails
        Host->>Host: K5010, the old version stays, no event
    end
```

A host with its own file watcher, or an editor "reload" button, calls `request_reload()`
directly and starts at step 7.
