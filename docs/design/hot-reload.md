# Hot reload (M5)

Status: **Proposed** (2026-09-27; landed the same day: `src/runtime/watch.cpp`, the reload path in `pump.cpp`, the source poller in `src/cook/provider.cpp`; verified end to end with `kiln-viewer --watch`). Updated 2026-09-30: the store is a catalog store (`store-catalog.md`); the runtime watches the catalog, not store files. Implements the reload rows of `handles-and-states.md`. Done when
editing a glb or PNG updates the viewer in about a second without leaks or crashes.

## Decision

Hot reload is two independent pollers joined by the store on disk:

1. **`kiln_runtime` watches the catalog.** Artifacts never change, so a poll thread stats the
   profile's catalog file. When a new valid catalog appears, the pump thread swaps it in and reloads
   each loaded asset whose entry names another artifact than the one it loaded or tried. This alone
   covers "someone re-ran `kiln-cook`" and any external pipeline, and needs no cook code.
2. **`kiln_cook`'s provider watches sources.** The provider watches every input file its cooks
   recorded this session (sources, sidecars, a `.gltf`'s buffers). A poll thread stats them; it
   re-cooks the changed sources of one round, publishes their artifacts and rewrites the catalog
   once, which the runtime poller then sees.

Neither poller knows about the other. Dependencies exist only on the cook side and only for the
v0.5 relation: glb to its embedded textures.

The cooker may also run in another process: `kiln-cook` (once, or `--watch` to keep cooking what
changes) writes the store while a read-only app (no provider) watches the catalog. One writer per
profile holds the lock, so an app with a disk-mode provider and `kiln-cook` do not share a store
(K3009).

### Runtime

- `ContextDesc::hotReload = { watchStore, pollMs }`. `watchStore` starts the store poller
  (compiled only with `KILN_HOT_RELOAD`; asking for it in a shipping build is K5011, a warning, and
  nothing is watched). Default off; `pollMs` 250.
- `request_reload(ctx, handle)` is the public entry the poller also uses: hosts with their own
  watcher or an editor "reload" button call it directly. It works without `KILN_HOT_RELOAD`.
- **Reload keeps the old payload servable.** A `Ready` asset stays `Ready`; `is_ready`, `gpu_object()`,
  `mesh_view()` and `texture_info()` keep answering with the current version until the swap. The
  meta stage writes into a second metadata set in the slot (`next`), the upload stage into a new
  target object, and the swap in `pump()` moves `next` to `cur`, increments the content version,
  binds a bindless slot to the new object, releases the old object after the host's frames that
  used it (`adapter-frames-slots.md`) and emits `Changed`. The first load uses the same path: `next` is filled, then swapped in with `Ready`.
- **A failed reload changes nothing** except one Error diagnostic (K5010) and the `next` set being
  discarded. A `Failed` asset that reloads successfully becomes `Ready` with `Ready` (not
  `Changed`); one that fails again stays `Failed` and emits `Failed` with the new status.
- Reload never emits `MetaReady` and never touches the handle generation. A reload of a `Ready`
  asset never touches load group counters (the group already counted it); a `Failed` asset that
  reloads into `Ready` is its first success, so its group moves it from `failed` to `ready` and
  adds its bytes to `bytesTotal` and `bytesDone`, as a first `Ready` would.
- A reload requested while a load is in flight is remembered and runs after that load settles. A
  memory-registered asset has no file to reload from: K5012, warning, ignored.
- `IoBackend::stat` is a new optional entry point (`IoStat { size, mtimeNs }`). The compat backend
  implements it; a host backend that leaves it null gets K5011 when `watchStore` is on. The compat
  backend opens files with `FILE_SHARE_DELETE` on Windows so a rewrite by rename can replace a file
  the loader is reading.
- The poller stats the catalog file, sleeps `pollMs` between rounds, reads and validates a changed
  catalog, and hands it to `pump()` under a mutex; the pump thread swaps it in and reloads. It is
  joined by `destroy()`. Without the poller, a reload reads the catalog again when it starts, so
  `request_reload` sees a catalog rewritten meanwhile.

### Cook side

- `ProviderDesc::watchSources` and `pollMs` start the source poller in `install_provider`.
- The provider's input records (`store-catalog.md`) hold, per source, every input with its size,
  time and content hash, and the outputs. A re-cook runs on the poller thread, so a glb publishes
  its mesh and its embedded images again; an image its texture.
  Diagnostics from a re-cook go to the log (`KILN_WARN` / `KILN_ERROR`), since there is no pump
  thread to replay them on.
- Artifacts are written once; only the catalog is replaced (a temporary file, then a rename). A
  failed rewrite is tried again next round.
- The poller is joined by `uninstall_provider`, which the host calls before `destroy(ctx)`.

### Viewer and examples

`kiln-viewer --watch` sets both flags and logs `Changed` events with the new version. Textures the
changed mesh newly references are requested as for a first load; pipelines for a new vertex layout
are created the same way. `kiln-headless --watch` does the same without a GPU.

### Diagnostics

| Code | Severity | Meaning |
|---|---|---|
| K5010 | Error | reload failed; the previous version stays |
| K5011 | Warning | hot reload unavailable: not compiled in, or the IO backend has no `stat` |
| K5012 | Warning | reload requested for a memory-registered asset |

## Rationale

- Two pollers joined by the catalog keep `kiln_runtime` free of any notion of sources and keep the
  cook side free of any notion of slots. Either half is useful alone.
- Swapping through a `next` metadata set makes first load and reload one code path and guarantees
  the old view stays valid until `Changed`.
- Polling is the simplest correct implementation on every platform; native watchers are a v0.9 item
  behind the same API.

## Alternatives considered

| Alternative | Why not |
|---|---|
| Runtime watches sources and forces a cook | The runtime would need source paths and the provider's dependency knowledge; breaks the shipping split. |
| Reload through `Pending` | Flickers to the placeholder and drops `is_ready`; the design note rejected it. |
| Native file watchers now | More code per platform for the same API; polling meets the one-second target. |

## Consequences / what this constrains later

- Fuller dependency tracking (v0.6) extends the provider's records; the runtime side is unchanged.
- Staleness at first load (source edited while the app was closed) is a provider check before
  load, not part of M5.
- Progressive loads (v0.8) reuse the `next` set for partial swaps.

## Open points for the owner

- Default `pollMs` of 250 for both pollers.
- Whether `request_reload` should also accept an `AssetId` for hosts that do not keep handles.
