# Runtime texture arrays from independent assets

**Status:** First version implemented and decided (v0.7, 2026-09-30; owner): one aggregate upload; the
other choices are open-questions R12. `kiln-gl-array`, `kiln-vk-array` (descriptor sets or bindless),
`kiln-sokol-array` and `kiln-nga-array` show it, and their `--verify` checks it on the GPU. Range
uploads (v0.8) and benchmarks are still open.
**Decides:** How a runtime request can assemble independently cooked 2D textures into one GPU
array, without producing a combined cooked file or changing the one-source-file rule.

## Problem and proposed outcome

Today an array comes from a single strip image or an existing array KTX2 file
([texture-shapes.md](texture-shapes.md)). A host with separate terrain tiles, animation frames,
or material textures must combine them outside kiln before cooking.

Add an explicit runtime request that declares an ordered list of texture asset names. Each
file is cooked, cached and addressed independently. The runtime validates their metadata,
allocates one array, and loads each file into its assigned layer. The resulting array has a
normal texture handle, readiness state and GPU binding, but no manifest entry or combined
artifact on disk. Layer order is fixed for the lifetime of the request.

The preferred path uploads directly into the final array. It does not first request ordinary
resident 2D textures and copy them. That avoids allocating both source images and the array.
There is no runtime BC encoding: cooked blocks are uploaded unchanged after any outer Zstd
decompression. Normal CPU metadata, I/O buffers and temporary upload staging remain necessary;
"GPU-only" means no persistent CPU pixel copy and no generated combined file.

## Existing foundations and missing pieces

- `include/kiln/adapter.h` already has `TextureShape::Array`, array capability reporting,
  layer/mip descriptions and shape-aware bindings.
- Array placeholders and shape checks already exist. `TextureHandle`, `texture_info()`,
  `gpu_object()`, groups, events and frame-safe destruction provide the public lifecycle.
- `src/runtime/loader.cpp` currently reads one source and calls `begin_upload()` to create
  one complete GPU object. There is no operation to upload one source into a layer of an
  existing object, nor an aggregate request spanning several source records.
- The store and cook provider can continue resolving each source normally. Runtime dependency
  tracking must work even when a source has no ordinary resident texture handle.

This is primarily a loader, ownership and adapter extension. It requires no new texture file
format and no change to the cooker's source dependency model.

## Proposed public contract

Illustrative signature, subject to the identity and adapter decisions below:

```cpp
TextureHandle request_texture_array(
    Context* ctx,
    StrView runtimeName,
    Span<StrView const> layers,
    RequestOptions const& options = {});
```

For example, `runtimeName = "terrain/tiles"` and the ordered names `"terrain/grass"`,
`"terrain/rock"`, `"terrain/sand"` produce layers 0, 1 and 2. The declaration lives in the
host application; it is not a new source file to cook.

Proposed rules:

- Copy the name/list at request time; caller storage need not survive the call. Resolve names
  using the existing asset-name rules. Reject an empty list or invalid name synchronously.
- Reserve `runtimeName` in the context's existing asset identity space, without putting it in
  the store. Reject a conflicting file-backed or memory-registered identity. Repeating an
  identical declaration returns the same refcounted handle; a different ordered list under the
  same name is an error, not an implicit edit. Compare declarations, not only their hashes.
- The returned texture is always `TextureShape::Array`. Prefer a dedicated options type or
  explicitly define `RequestOptions::textureShape` as unused for this API; its existing default
  is `Tex2D`, so requiring callers to override it would be surprising.
- Use the existing request priority, group and placeholder-kind conventions. Expose metadata
  only after all layers validate. Become Ready only after all required uploads finish.
- Initially accept file-backed 2D assets only. Sources cannot be arrays, cubes or other runtime
  composites. Duplicate source names are allowed and occupy distinct layers.
- An ordinary request for a source remains independent and may allocate another GPU image.
  This API does not relocate or change the identity of existing source handles.
- Runtime arrays are recreated from the host's declarations each session. They are not included
  in store export or garbage-collection roots as persistent artifacts. Exporting a package must
  still include all independently named source assets.

The named identity is a proposal, not a requirement of GPU composition. An anonymous handle
keyed by the ordered source list is an alternative discussed under Open points.

## Compatibility and validation

For the first implementation, every source must have the same cooked width, height, exact
format (including sRGB/linear and signedness), and mip count. Every source is a single-layer,
single-face 2D texture. Check the resolved cooked metadata, not source extensions or settings.
Reject mismatches with the source name, layer index and expected/actual values.

Do not resize, transcode, synthesize missing mips or reinterpret formats at load time. A host
must choose compatible cook settings for an intended array. All mip levels are copied; BC
tail mips and block alignment follow the existing format layout helpers.

Validate layer count, dimensions and resource size against adapter/device limits before
expensive data reads. `kArrayTextures` establishes shape support, but does not currently report
numeric limits or incremental upload support. A cooked target's layer limit is not a substitute
for checking the device. Allocation failure remains possible after validation.

Array layers share the resource's format and mip structure. Sampler behavior is selected by the
host's binding; the declaration does not preserve a separate sampler per source.

## Load flow and memory

1. Create the aggregate request and its array placeholder binding.
2. Resolve each source through the normal manifest/cook-provider path. Capture the artifact key
   and validated metadata for the version being loaded.
3. Validate the complete declaration and compute checked layout/size values.
4. Reserve the array object and upload resources. Read/decompress each source into the upload
   destination for its layer and mip. Apply existing I/O, staging and per-pump budgets.
5. Poll all submitted work. Publish one object and one Ready event only when the complete array
   is valid. Release temporary source buffers and upload reservations after their work completes.

Do not implement step 2 by calling `request_texture()` for every source: that would also
schedule their standalone GPU uploads. Reuse or factor the source-resolution/metadata work
separately from GPU residency. Within a load attempt, pin each source's artifact key so metadata
and payload cannot silently come from different versions. There is no promise of one atomic
version across all source files; a later change schedules another aggregate load.

The final texel payload is approximately the sum of the source payloads, plus implementation
alignment/metadata overhead. Direct layer uploads permit bounded temporary staging. A backend
that requires complete initial data may need staging as large as the whole array; document and
budget that cost rather than promising constant scratch space on every adapter.

An array exceeding a fixed staging capacity must either use supported chunked uploads or fail
with an actionable size diagnostic. It must not return Busy forever for an impossible request.

## Adapter design

Separate ownership of the array allocation from completion of individual writes. The current
contract gives every upload its own object and lets kiln destroy that object after completion;
reusing it unchanged for several layers would risk double destruction or early publication.

Two implementation choices should be evaluated in a spike:

| Contract | Benefits | Cost |
|---|---|---|
| Allocate texture, then upload explicit mip/layer ranges into it | Bounded staging; natural future partial uploads | New object ownership, upload tickets and synchronization rules |
| One aggregate upload with all layer data arranged by mip | Closest to today's adapter contract; supports creation-time initial data | Potentially whole-array staging; needs scatter destinations for independent source reads |

Prefer range uploads for native backends, with an explicitly budgeted aggregate path where
needed. Exact callback signatures and capability bits remain open. Do not silently turn a
GPU-copy-only request into CPU readback, or claim range support from `kArrayTextures` alone.

Workers may prepare data under the existing threading contract. GPU submission must continue
respecting each adapter's thread requirements, including GL context-thread work through `flush()`.
An object is owned by the aggregate request; upload tickets borrow it. Cancellation cannot free
the object until every submitted ticket is terminal. Unsubmitted reservations are discarded.

Current backend implications:

| Backend | Direct assembly | Existing-resident copy alternative |
|---|---|---|
| Vulkan | One array image; buffer-to-image copies select destination layers/mips | `vkCmdCopyImage`; the example adapter's images have transfer-source usage (for `--verify`), but synchronization must change |
| OpenGL | Array storage and subimage uploads; current adapter already uploads complete arrays | `glCopyImageSubData`; add entry point and completion/lifetime handling |
| sokol | The aggregate upload is one `sg_make_image` (first version: `kiln-sokol-array`, D3D11 byte-exact); range uploads would need `sg_update_image`, which replaces whole images | Native API support does not establish support through sokol; investigate before exposing this capability |
| NoGraphicsAPI | The aggregate upload is one copy per level (first version: `kiln-nga-array`, built, not run yet); `copy_memory_to_texture` takes a slice range, so range uploads fit | Needs a separate capability audit |
| Null | Model allocation, writes, completion and failure for deterministic tests | Optional simulated copies for contract tests |

No backend performance claim has been measured yet.

## Lifetime, errors and hot reload

The aggregate owns its source-dependency records and destination object. It does not need to
keep separate source GPU images alive. Shared source metadata can be refcounted independently
of any ordinary texture residency.

On initial failure, keep the normal failed/placeholder behavior and report the failing source
and layer. Release all reservations after outstanding work finishes. Releasing the last handle
while loading cancels further reads where possible and drains submitted GPU work safely.

For the first version, reload transactionally: a change to any member builds a complete new
array, and the old one stays visible until the replacement is Ready. Publish one Changed event,
increment the version once, and retire the old array through the existing frame-completion rules.
An incompatible or failed replacement keeps the previous array. Coalesce changes that arrive
while a replacement is loading and recheck afterward.

Store polling must map changed source names/keys to dependent arrays even if those sources were
never requested individually. `request_reload()` on the aggregate rechecks every member through
the cook provider. Diagnostic codes will be assigned during implementation, following the
existing runtime error catalogue.

This policy temporarily needs both old and new arrays, plus staging. In-place layer replacement
would save memory but introduces writes to resources sampled by frames in flight; defer it until
there is an explicit synchronization and visibility contract. Layer count and order cannot be
edited through reload; a new declaration is required. Reloading only the changed layers (copy-on-write
by default, in place as an opt-in) is open for v0.8 with range uploads: open-questions R12.

## Alternatives and GPU constraints

**Copy already-loaded textures into a new array.** Useful when source handles are already
resident. Allocate the destination and copy every mip into its layer on the GPU, retaining
source objects until copies finish. This needs no CPU readback or BC recompression, but adds
bandwidth and destination storage. Sources can be released only if other owners no longer need
them. Treat this as a separate later API, not the default implementation of loading from names.

**Bind a descriptor array of independent 2D textures.** Avoids copying and permits different
dimensions/formats where the renderer supports the required bindings. This is a different shader
interface from sampling a true texture array. Kiln's bindless slots help hosts implement it,
but do not make independent allocations into array layers.

**Share views of one allocation.** A view selects subresources of one existing image; it cannot
portably concatenate unrelated images. Allocating an array first and exposing 2D views of its
layers can share storage on supporting backends, but adds parent/child lifetime and reload
semantics. Defer automatic sharing with ordinary texture handles.

**Combine files during cooking.** Produces one persistent asset and simpler runtime loading,
but adds multi-source cooking and invalidation. It does not address the requested runtime-only
composition while retaining the existing cook rule.

Sparse memory aliasing is not a portable shortcut for joining arbitrary existing texture
allocations. It would require a separate residency design and device-specific constraints.

## Validation and rollout proposal

1. Resolve identity, options, adapter ownership and staging-budget questions below.
2. Prototype the metadata-only source path and aggregate lifecycle with the null adapter.
   Check ordered/duplicate members, identity conflicts, metadata mismatches, missing files,
   failure cleanup, release during upload, and source invalidation without resident 2D handles.
3. Implement direct assembly in Vulkan and GL. Verify every layer/mip against loading each source
   independently, including BC tail mips, sRGB and Zstd. Use test-only readback and API validation
   to detect layout, synchronization and lifetime errors.
4. Validate reload success/failure while frames are in flight and bounded staging with a large
   array. Confirm no individual source GPU allocations and no combined store artifact are made.
5. Audit sokol and NoGraphicsAPI; implement supported paths and reject unsupported capabilities
   explicitly. Document any whole-array staging requirement.
6. Benchmark direct assembly against independent loads and a GPU-copy assembly prototype:
   time to Ready, CPU work, transferred bytes, GPU copy time, peak staging and peak GPU memory.
   Include cold/warm file reads and reload; do not equate allocation/copy feasibility with speed.

## First version (implemented)

- API: `request_texture_array(ctx, TextureArrayDesc{name, layers, textureKind, priority, group})`,
  released with `release(ctx, TextureHandle)`; every other texture query works on it.
- Identity: a caller-supplied name in the texture name space, K5020 on a conflict.
- Adapter: one upload with every layer (the second row of the adapter table above). No new entry
  point or capability: `kArrayTextures`, and staging for the whole array.
- Scope: store-entry 2D layers of one format, size and level count (K5021); whole-array reload.
- Hot reload: each layer keeps its artifact key; a manifest change to any layer reloads the array.

The open points below record what the first version chose; R12 has the details.

**Validation (2026-09-30).** Null-adapter tests cover the layout (including BC tail levels and
padded rows), mixed Zstd and plain layers, release during a load, `destroy()` with a live array,
an array larger than staging, reload merging and layers cooked on a miss. On the GPU,
`kiln-gl-array --verify` and `kiln-vk-array --verify [--bindless]` read the array back and compare
every layer and level with the tile loaded on its own: byte-exact for 6 BC7 layers of 8 levels on
an NVIDIA GTX 1080 Ti, with a warm and a cold store. Not run with the Vulkan validation layer.

## Open points

- **Identity:** accept the proposed caller-supplied runtime name, or return anonymous handles
  deduplicated by the ordered source list? Named requests fit current lookup/events but need
  explicit collision behavior and treatment by name queries.
- **Options:** use a dedicated array request options struct to avoid `textureShape` ambiguity?
- **Adapter contract:** adopt allocation plus range writes, or start with aggregate uploads?
  How are device limits and maximum staging capacity exposed without overloading existing caps?
- **Memory budget:** what cap applies to aggregate declarations, dependency records and staged
  bytes? How does it interact with `maxAssets`, concurrent arrays and transactional reloads?
- **Initial scope:** ship only file-backed 2D members and full-array transactional reload, as
  proposed, or require memory-registered sources / resident-handle copies in the first version?

## Research references

- [Vulkan image views](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewCreateInfo.html):
  a view references one image and a subresource range.
- [Vulkan image copies](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyImage.html):
  copies between image subresources, subject to usage, layout and format requirements.
- [OpenGL texture views](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_texture_view.txt):
  shared storage views of an existing immutable texture.
- [OpenGL image copies](https://registry.khronos.org/OpenGL/extensions/ARB/ARB_copy_image.txt):
  GPU image copies, including array slices and compatible compressed formats, without resampling
  or on-the-fly compression/decompression.
- [D3D11 subresource copies](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copysubresourceregion):
  underlying API support for the resident-copy approach; not a guarantee of wrapper support.
