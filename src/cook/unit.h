// src/cook/unit.h — one cook of one source file: every output it made and every input file it
// read (docs/design/store-manifest.md). Shared by the cook provider and kiln-cook.
#pragma once

#include "kiln/cook/cook.h"
#include "kiln/cook/manifest.h"
#include "kiln/io.h"

namespace kiln::cook {

/// Strings are offsets into CookUnit::strings, which may grow.
struct UnitInput {
    InputRole role = InputRole::Source;
    u32 nameOff = 0, nameLen = 0; ///< BuildInput::name
    u32 pathOff = 0, pathLen = 0; ///< the file as the cook opened it; empty in a record (input_path())
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

    explicit CookUnit(Allocator const* a)
        : inputs(a, Tag::Cook), outputs(a, Tag::Cook), strings(a, Tag::Cook) {}

    StrView str(u32 off, u32 len) const { return {strings.data() + off, len}; }
    StrView name(UnitOutput const& o) const { return str(o.nameOff, o.nameLen); }
    /// The output called `name` of `kind`, or null.
    UnitOutput* find(AssetKind kind, StrView name);
    /// The first output that failed, else Ok.
    Status first_failure() const;
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
Status cook_unit(UnitDesc const& d, CookUnit* out);

/// The build inputs of `unit`, for build_key(). `out` must hold unit.inputs.size() entries.
void unit_build_inputs(CookUnit const& unit, BuildInput* out);

/// What the host sets for every asset: cooker version, target, default settings, name rules, the
/// session and `policyVersion`. Another digest means the recorded keys must be checked again.
u64 host_digest(UnitDesc const& d, u32 policyVersion);

/// True if the settings of `d` give every output of the recorded unit `rec` the key it has
/// (copy_input_record()): the settings are resolved again over the recorded inputs, reading only
/// the sidecar next to `d.sourcePath`. False when an output has no key or the sidecar cannot be read.
[[nodiscard]] bool recorded_keys_match(UnitDesc const& d, CookUnit const& rec);

/// The file of a recorded input, from the unit's source as it is found now: the source itself, its
/// `.kiln` file, or a buffer URI relative to the source's directory. Records keep no paths, so a
/// store stays valid when the sources move. Returns what `format()` returns.
usize input_path(StrView sourcePath, InputRole role, StrView name, char* out, usize cap);

enum class InputsCheck : u8 {
    Unchanged, ///< every input has its recorded size and time (an absent sidecar is still absent)
    Touched,   ///< some size or time differs, but every content hash matches: `rec` has the new stats
    Changed,   ///< some content differs, or a file appeared or went missing
};

/// Compares the recorded inputs of `rec` with the files next to `sourcePath`. A file whose size or
/// time differs is read and hashed before it counts as changed; with `rehash`, every file is.
[[nodiscard]] InputsCheck check_recorded_inputs(CookUnit& rec, StrView sourcePath, bool rehash = false);

/// Size and modification time of a file, through the compat backend's stat when it has one.
Status stat_file(StrView path, IoStat* out);

} // namespace kiln::cook
