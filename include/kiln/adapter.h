// kiln/adapter.h — the renderer boundary: function pointers the host renderer fills in.
// kiln never calls a graphics API. See docs/design/adapter.md.
#pragma once

#include "kiln/formats.h"
#include "kiln/result.h"

namespace kiln {

/// FNV-1a 64 of the asset name (`asset_id()` in kiln/assets.h).
using AssetId = u64;

/// Ids 1..15 are reserved for the built-in placeholders (handles-and-states.md).
inline constexpr AssetId kFirstPlaceholderId = 1;
inline constexpr AssetId kLastPlaceholderId  = 15;

enum class UploadKind : u8 { MeshPayload = 0, TextureLevels = 1 };

/// Selects the placeholder served while a texture is Pending or Failed.
enum class TextureKind : u8 { BaseColor = 0, Normal, Orm, Emissive, Count };

/// The image type of a texture (docs/design/texture-shapes.md). A cube's 6 faces arrive as
/// `TextureDesc::layers == 6`, in the order +X, -X, +Y, -Y, +Z, -Z.
enum class TextureShape : u8 { Tex2D = 0, Cube, Array, Count };
/// "2D", "cube", "array"; "unsupported" for Count (a volume or a cube array).
[[nodiscard]] KILN_API char const* texture_shape_name(TextureShape s) noexcept;

enum AdapterCaps : u32 {
    /// commit_upload submits to a queue by itself and is_upload_complete makes progress
    /// without the host recording a frame. wait() needs this bit or Adapter::flush.
    kSelfSubmitting = 1u << 0,
    /// The adapter accepts TextureShape::Cube / Array. Without the bit, kiln uploads no
    /// placeholder of that shape and a request for it fails (K5004).
    kCubeTextures  = 1u << 1,
    kArrayTextures = 1u << 2,
    /// The adapter accepts UploadKind::MeshPayload. Without the bit, kiln never uploads a mesh,
    /// and a mesh request fails (K5004).
    kMeshes = 1u << 3,
    // bits 4..31 reserved, must be 0
};

struct CopyConstraints {
    u64 optimalRowPitchAlign = 1;   ///< row pitch kiln honors when laying out texture levels
    u64 optimalOffsetAlign   = 16;  ///< per-level offset alignment inside a texture upload
    u64 bufferOffsetAlign    = 256; ///< alignment of a mesh payload upload start
};

/// What kiln is about to place for a texture. Levels are uploaded in ascending
/// level order, each starting at optimalOffsetAlign, rows padded to rowPitchAlign.
struct TextureDesc {
    Format format      = Format::Undefined;
    u32 width          = 0;
    u32 height         = 0;
    u32 depth          = 1;
    u32 layers         = 1; ///< array layers, or 6 for a cube
    u32 levels         = 1;
    TextureShape shape = TextureShape::Tex2D;
    u32 firstLevel     = 0; ///< reserved for partial loads (v0.8); 0 in v0.5
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

/// Every function pointer except `bind` and `flush` must be set. `begin_upload` /
/// `commit_upload` may run on kiln worker threads, everything else runs on the pump thread
/// (threading contract and frames: docs/design/adapter.md, docs/design/adapter-frames-slots.md).
struct Adapter {
    /// Answers for the whole host: the API's support, and for VertexBuffer also what the host's
    /// shaders read (with vertex pulling, only the shaders decide).
    bool (*supports_format)(void* user, Format f, FormatUsage usage) = nullptr;
    void (*copy_constraints)(void* user, CopyConstraints* out)       = nullptr;
    /// May return Code::Busy (back-pressure); kiln retries on a later pump.
    Status (*begin_upload)(void* user, UploadDesc const& desc, UploadTarget* out) = nullptr;
    void (*commit_upload)(void* user, u64 token)                                  = nullptr;
    /// Polled on the pump thread until true. After true, kiln uses the object (or destroys it if the
    /// asset was dropped meanwhile).
    bool (*is_upload_complete)(void* user, u64 token) = nullptr;
    /// Bindless adapters: slot `slot` (kiln numbers them, [0, bindlessSlots)) shows `obj` from now
    /// on: the placeholder of the texture's kind and shape at the request, the texture once Ready,
    /// the new one after a reload, the Failed checker with devPlaceholders.
    void (*bind)(void* user, u32 slot, GpuObject obj, TextureShape shape) = nullptr;
    /// No frame the host reported (PumpOptions::frame / completedFrame) can use `obj` any more:
    /// free it now. Its upload has completed.
    void (*destroy)(void* user, GpuObject obj) = nullptr;
    /// Optional. Called at the start of every pump() (so in every wait() loop too) and while
    /// create() waits for the placeholders, on that thread. An adapter whose GPU work must run
    /// on the graphics context's thread does it here; the host then calls pump() on that thread.
    void (*flush)(void* user) = nullptr;
    u32 caps                  = 0;  ///< AdapterCaps
    u32 bindlessSlots         = 0;  ///< with `bind`: the slots kiln may hand out; 0 = not bindless
    void* reserved[4]         = {}; ///< residency hooks (v0.8); must be null
    void* user                = nullptr;
};

/// Byte offset and row pitch of each level inside a texture upload, as kiln writes it for these
/// copy constraints: levels ascending, each starting at optimalOffsetAlign, rows padded to
/// optimalRowPitchAlign, the layers (cube faces) of a level one after another. `offsets` and
/// `pitches` receive `t.levels` entries each; either may be null. Returns the upload size.
[[nodiscard]] KILN_API u64 texture_level_layout(TextureDesc const& t, CopyConstraints const& c, u64* offsets,
                                                u64* pitches) noexcept;

/// True if every required entry point is set, `bind` comes with `bindlessSlots`, and the reserved
/// tail is null.
[[nodiscard]] constexpr bool adapter_is_valid(Adapter const& a) noexcept {
    return a.supports_format && a.copy_constraints && a.begin_upload && a.commit_upload &&
           a.is_upload_complete && a.destroy && (a.bind != nullptr) == (a.bindlessSlots != 0) &&
           !a.reserved[0] && !a.reserved[1] && !a.reserved[2] && !a.reserved[3] &&
           (a.caps & ~u32(kSelfSubmitting | kCubeTextures | kArrayTextures | kMeshes)) == 0;
}

} // namespace kiln
