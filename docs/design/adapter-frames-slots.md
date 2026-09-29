# Frames and slots at the adapter boundary

**Status:** Decided (owner, 2026-09-28: "A full" of the integration review). Implemented.
**Decides:** kiln learns when the host's frames finish, frees GPU objects only when no frame can use
them, and numbers bindless slots itself. `acquire`, `publish` and `destroy_deferred` give way to
`bind` and `destroy`.

## Why

The integration examples (`integration-examples.md`, `../api-friction.md`) wrote the same
bookkeeping in every adapter that keeps objects past a frame: an asset-id → slot map under a mutex,
placeholder tracking for `acquire`, a retire list with its own frame signal, and cleanup of uploads
kiln abandoned at unload. About a third of the Vulkan, bindless GL and NoGraphicsAPI adapters. All
of it follows from two gaps: kiln did not know about frames, and it kept slot numbers to the
adapter although it tracks every asset's lifetime.

## Decision

### 1. The host reports frames

`PumpOptions` gains two counters, both starting at 1; 0 keeps the last value reported:

- `frame`: the frame the host records after this pump.
- `completedFrame`: every frame up to this one has finished on the GPU.

An object kiln stops using during a pump is released once `completedFrame` reaches that pump's
`frame`. A host that never reports keeps both at 0, and kiln releases at once: right for APIs that
keep objects alive for issued commands (GL with bound textures, sokol), for the null adapter and for
tools. `wait()` pumps with the last values.

### 2. `destroy` replaces `destroy_deferred`

`void (*destroy)(void* user, GpuObject obj)`: no frame the host reported can use `obj`; free it now.
kiln also finishes uploads it abandons: when an asset is unloaded or fails with its upload in
flight, kiln keeps polling that token and destroys the object after the upload completed and the
frames passed. No adapter retires anything by itself any more.

### 3. kiln numbers bindless slots

- `Adapter::bindlessSlots` (0: not bindless) says how many slots the adapter has; kiln hands out
  `[0, bindlessSlots)`, one per texture asset, and reuses a number only after the frames that could
  read it finished (the same rule as objects).
- `void (*bind)(void* user, u32 slot, GpuObject obj, TextureShape shape)`: slot `slot` shows `obj`
  from now on. kiln calls it at the request (the placeholder of the texture's kind and shape), when
  the texture is Ready, after each reload, and with the Failed checker (`devPlaceholders`). It runs
  on the pump thread (requests are pump-thread calls).
- `gpu_object()` returns the object with `slot` set to kiln's number, from the request on. A request with
  every slot in use fails with K5004.
- If the placeholder's upload has not completed at the request (an adapter with neither
  `kSelfSubmitting` nor `flush`), kiln binds the slot as soon as it completes.
- An adapter whose API forbids rewriting a descriptor that in-flight frames may read keeps a table
  (`kiln-nga`); `bind` then updates the table, not a descriptor.

### 4. What goes

- `acquire`: `bind` with the placeholder object replaces it; the adapter no longer needs to know
  placeholder ids.
- `publish`: `bind` covers bindless adapters; others never needed it. An adapter that wants to know
  when an upload is used (the Vulkan watermark) learns it in `is_upload_complete`: kiln stops
  polling a token at its first true and uses the object from then on (or destroys it, if the asset
  was dropped meanwhile).
- `destroy_deferred`, `null_adapter_flush_deferred`.

## Consequences

- Adapter API break (recorded in `CHANGELOG.md`). Migration: move `acquire` + `publish` into `bind`,
  make `destroy_deferred` an immediate `destroy`, delete retire lists and id → slot maps, and report
  frames from the host's loop.
- `destroy(ctx)` destroys everything at once: the host waits for its GPU to go idle first (as
  before). It still waits for abandoned uploads to complete, polling with `flush`.
