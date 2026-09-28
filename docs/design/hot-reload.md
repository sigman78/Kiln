# Hot reload (M5)

Status: **Proposed** (2026-09-27; landed the same day: `src/runtime/watch.cpp`, the reload path in `pump.cpp`, the source poller in `src/cook/provider.cpp`; verified end to end with `kiln-viewer --watch`). Implements the reload rows of `handles-and-states.md`. Done when
editing a glb or PNG updates the viewer in about a second without leaks or crashes.

## Decision

Hot reload is two independent pollers joined by the store on disk:

1. **`kiln_runtime` watches the store.** For every loaded file-backed asset it remembers the size
   and modification time of the cooked file it loaded. A poll thread stats those files; when one
   changes, the asset reloads from the store. This alone covers "someone re-ran `kiln-cook`" and any
   external pipeline, and needs no cook code.
2. **`kiln_cook`'s provider watches sources.** The cook-on-miss provider remembers which source file
   produced each asset it cooked (a glb produces the mesh and its embedded textures; a PNG or KTX2
   produces one texture). A poll thread stats those sources; when one changes it re-cooks through the
   same functions as cook-on-miss and rewrites the store files, which the runtime poller then sees.

Neither poller knows about the other. Dependencies exist only on the cook side and only for the
v0.5 relation: glb to its embedded textures.

### Runtime

- `ContextDesc::hotReload = { watchStore, pollMs }`. `watchStore` starts the store poller
  (compiled only with `KILN_HOT_RELOAD`; asking for it in a shipping build is K5011, a warning, and
  nothing is watched). Default off; `pollMs` 250.
- `request_reload(ctx, handle)` is the public entry the poller also uses: hosts with their own
  watcher or an editor "reload" button call it directly. It works without `KILN_HOT_RELOAD`.
- **Reload keeps the old payload servable.** A `Ready` asset stays `Ready`; `is_ready`, `gpu()`,
  `mesh_view()` and `texture_info()` keep answering with the current version until the swap. The
  meta stage writes into a second metadata set in the slot (`next`), the upload stage into a new
  target object, and the swap in `pump()` moves `next` to `cur`, increments the content version,
  calls `publish(id, obj, version)`, passes the old object to `destroy_deferred` and emits
  `Changed`. The first load uses the same path: `next` is filled, then swapped in with `Ready`.
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
- The poller stats only settled slots (`Ready` or `Failed`, file source), sleeps `pollMs` between
  rounds, and hands changed slot indices to `pump()` through a mutex-protected list; the pump thread
  calls `request_reload`. It is joined by `destroy()`.

### Cook side

- `ProviderDesc::watchSources` and `pollMs` start the source poller in `install_provider`.
- The provider records per cooked asset: source path, its `IoStat` at cook time, and the asset paths
  it produced. Re-cook happens on the poller thread through the existing cook-on-miss functions with
  `storeMode` Disk, so a glb rewrites its mesh and its embedded textures; a PNG rewrites its texture.
  Diagnostics from a re-cook go to the log (`KILN_WARN` / `KILN_ERROR`), since there is no pump
  thread to replay them on.
- `store_write` gains an `overwrite` flag. Cook-on-miss keeps the existing leave-if-present rule;
  re-cook overwrites. On Windows a rename over a file that a reader still holds open without
  `FILE_SHARE_DELETE` fails; the poller keeps the source marked dirty and tries again next round.
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

- Two pollers joined by the store keep `kiln_runtime` free of any notion of sources and keep the
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
