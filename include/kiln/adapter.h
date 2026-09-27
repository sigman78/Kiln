// kiln/adapter.h — the renderer boundary. kiln never calls a graphics API; every
// GPU interaction goes through this struct of function pointers that the host
// renderer fills in. Design: docs/design/adapter.md (HANDOFF §6.4).
#pragma once

#include "kiln/formats.h"
#include "kiln/result.h"

namespace kiln {

/// FNV-1a 64 of the normalized cooked asset path (forward slashes, no extension).
using AssetId = u64;

/// Ids 1..15 are reserved for the built-in placeholders (handles-and-states.md).
inline constexpr AssetId kFirstPlaceholderId = 1;
inline constexpr AssetId kLastPlaceholderId  = 15;

enum class UploadKind : u8 { MeshPayload = 0, TextureLevels = 1 };

/// Selects the placeholder served while a texture is Pending or Failed.
enum class TextureKind : u8 { BaseColor = 0, Normal, Orm, Emissive, Count };

enum AdapterCaps : u32 {
    /// commit_upload submits to a queue by itself and is_upload_complete makes progress
    /// without the host recording a frame. Required by wait() (HANDOFF §6.4).
    kSelfSubmitting = 1u << 0,
    // bits 1..31 reserved, must be 0
};

struct CopyConstraints {
    u64 optimalRowPitchAlign = 1;   ///< row pitch kiln honors when laying out texture levels
    u64 optimalOffsetAlign   = 16;  ///< per-level offset alignment inside a texture upload
    u64 bufferOffsetAlign    = 256; ///< alignment of a mesh payload upload start
};

/// What kiln is about to place for a texture. Levels are uploaded in ascending
/// level order, each starting at optimalOffsetAlign, rows padded to rowPitchAlign.
struct TextureDesc {
    Format format  = Format::Undefined;
    u32 width      = 0;
    u32 height     = 0;
    u32 depth      = 1;
    u32 layers     = 1;
    u32 levels     = 1;
    u32 firstLevel = 0; ///< reserved for partial loads (v0.8); 0 in v0.5
};

/// What kiln is about to place for a mesh payload (.mesh header values).
struct MeshPayloadDesc {
    u64 payloadDecodedSize = 0;
    u32 payloadAlignment   = 256;
    u32 indexSize          = 4; ///< largest index size used (1, 2 or 4)
};

struct UploadDesc {
    AssetId id                  = 0;
    UploadKind kind             = UploadKind::MeshPayload;
    u64 size                    = 0; ///< bytes kiln will write into UploadTarget::dst
    u32 alignment               = 16;
    TextureDesc const* texture  = nullptr; ///< non-null for TextureLevels
    MeshPayloadDesc const* mesh = nullptr; ///< non-null for MeshPayload
};

/// Adapter-defined, opaque to kiln. All-zero `native` with `slot == kInvalid` is null.
struct GpuObject {
    u64 native = 0;        ///< image/buffer pointer, handle or index
    u32 slot   = kInvalid; ///< bindless descriptor slot, if the adapter uses them
    u32 kind   = 0;        ///< adapter-defined tag

    [[nodiscard]] constexpr bool is_null() const noexcept { return native == 0 && slot == kInvalid; }
};

struct UploadTarget {
    void* dst         = nullptr; ///< mapped staging / ReBAR / scratch, >= UploadDesc::size bytes
    u64 rowPitchAlign = 1;       ///< row pitch kiln must honor for this upload
    u64 token         = 0;       ///< renderer's opaque ticket, never 0
    GpuObject object;            ///< the object that holds the data once the upload completes
};

/// The renderer boundary. Every function pointer except `acquire` and `publish`
/// must be set. Threading (adapter.md): `acquire` runs on the requesting thread;
/// `begin_upload` / `commit_upload` may run on kiln worker threads; everything else
/// runs on the pump thread.
struct Adapter {
    bool (*supports_format)(void* user, Format f, FormatUsage usage) = nullptr;
    void (*copy_constraints)(void* user, CopyConstraints* out)       = nullptr;
    /// Called once per asset on its first request. Bindless adapters allocate a slot
    /// bound to the placeholder and return it; others may return a null object.
    Status (*acquire)(void* user, AssetId id, UploadKind kind, TextureKind texKind, GpuObject* out) = nullptr;
    /// May return Code::Busy (back-pressure); kiln retries on a later pump.
    Status (*begin_upload)(void* user, UploadDesc const& desc, UploadTarget* out) = nullptr;
    void (*commit_upload)(void* user, u64 token)                                  = nullptr;
    bool (*is_upload_complete)(void* user, u64 token)                             = nullptr;
    /// During pump(), when an asset becomes Ready or a hot reload swaps its payload.
    /// `version` is the content version. A null `obj` frees the asset's slot at unload.
    void (*publish)(void* user, AssetId id, GpuObject obj, u32 version) = nullptr;
    /// The renderer delays destruction by its frames in flight.
    void (*destroy_deferred)(void* user, GpuObject obj) = nullptr;
    u32 caps                                            = 0;  ///< AdapterCaps
    void* reserved[4]                                   = {}; ///< residency hooks (v0.8); must be null
    void* user                                          = nullptr;
};

/// True if every required entry point is set and the reserved tail is null.
[[nodiscard]] constexpr bool adapter_is_valid(Adapter const& a) noexcept {
    return a.supports_format && a.copy_constraints && a.begin_upload && a.commit_upload &&
           a.is_upload_complete && a.destroy_deferred && !a.reserved[0] && !a.reserved[1] && !a.reserved[2] &&
           !a.reserved[3] && (a.caps & ~u32(kSelfSubmitting)) == 0;
}

} // namespace kiln
