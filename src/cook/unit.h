// src/cook/unit.h — one cook of one source file: every output it made and every input file it
// read (docs/design/store-catalog.md). Shared by the cook provider and kiln-cook.
#pragma once

#include "kiln/cook/catalog.h"
#include "kiln/cook/cook.h"
#include "kiln/io.h"

namespace kiln::cook {

/// Strings are offsets into CookUnit::strings, which may grow.
struct UnitInput {
    InputRole role = InputRole::Source;
    u32 nameOff = 0, nameLen = 0; ///< BuildInput::name
    u32 pathOff = 0, pathLen = 0; ///< the file as the cook opened it
    IoStat stat;                  ///< taken before the read; zeros when UnitDesc::statInputs is false
    Hash128 content;              ///< zero when absent
    bool present = true;          ///< false: a sidecar that does not exist, so creating it is a change
};

struct UnitOutput {
    u32 nameOff = 0, nameLen = 0;
    AssetKind kind   = AssetKind::Mesh;
    SlotHint slot    = SlotHint::None; ///< embedded images: the first slot that referenced them
    u64 settingsHash = 0;
    Hash128 key;
    Vec<u8> bytes;       ///< the cooked file; empty when `status` failed
    Status status = kOk; ///< only embedded images fail on their own
    CookStats stats;
};

struct CookUnit {
    Vec<UnitInput> inputs;
    Vec<UnitOutput> outputs; ///< a mesh first, then its embedded images in reference order
    Vec<char> strings;

    explicit CookUnit(Allocator const* a) noexcept
        : inputs(a, Tag::Cook), outputs(a, Tag::Cook), strings(a, Tag::Cook) {}

    [[nodiscard]] StrView str(u32 off, u32 len) const noexcept { return {strings.data() + off, len}; }
    [[nodiscard]] StrView name(UnitOutput const& o) const noexcept { return str(o.nameOff, o.nameLen); }
    /// The output called `name` of `kind`, or null.
    [[nodiscard]] UnitOutput* find(AssetKind kind, StrView name) noexcept;
    /// The first output that failed, else Ok.
    [[nodiscard]] Status first_failure() const noexcept;
};

struct UnitDesc {
    AssetKind kind     = AssetKind::Mesh; ///< Mesh: a glb/gltf source; Texture: an image of its own
    StrView name       = {};              ///< the source's asset name
    StrView sourcePath = {};
    MeshCookSettings const* meshDefaults       = nullptr;
    TextureCookSettings const* textureDefaults = nullptr;
    Span<NameRule const> nameRules             = {};
    CookPolicy policy                          = {};
    TargetProfile const* target                = nullptr;
    CookSession session                        = {};
    CookEnv env                                = {};
    bool statInputs                            = false;
};

/// Reads the source, its sidecar and any external buffers, and cooks every output. A mesh source
/// also cooks its embedded images. Fails when an input cannot be read or the first output fails;
/// an embedded image that fails keeps its Status in its output.
[[nodiscard]] Status cook_unit(UnitDesc const& d, CookUnit* out) noexcept;

/// The build inputs of `unit`, for build_key(). `out` must hold unit.inputs.size() entries.
void unit_build_inputs(CookUnit const& unit, BuildInput* out) noexcept;

/// What the host sets for every asset: cooker version, target, default settings, name rules, the
/// session and `policyVersion`. Another digest means the recorded keys must be checked again.
[[nodiscard]] u64 host_digest(UnitDesc const& d, u32 policyVersion) noexcept;

/// True if the settings of `d` give every output of the recorded unit `rec` the key it has
/// (copy_input_record()): the settings are resolved again over the recorded inputs, reading only
/// the sidecar. False when an output has no key or the sidecar cannot be read.
[[nodiscard]] bool recorded_keys_match(UnitDesc const& d, CookUnit const& rec) noexcept;

/// True if every recorded input still has its size and modification time (an absent sidecar is
/// still absent).
[[nodiscard]] bool recorded_inputs_unchanged(CookUnit const& rec) noexcept;

/// Size and modification time of a file, through the compat backend's stat when it has one.
[[nodiscard]] Status stat_file(StrView path, IoStat* out) noexcept;

} // namespace kiln::cook
