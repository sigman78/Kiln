// fuzz/fuzz_mesh_read.cpp — libFuzzer target: MeshView::open on arbitrary bytes, then the payload
// decode and the index check a tool would run on what it accepted.
#include <kiln/containers.h>
#include <kiln/mesh.h>

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size) {
    using namespace kiln;
    Span<u8 const> const bytes(data, size);
    Result<mesh::MeshView> const r = mesh::MeshView::open(bytes);
    if (r.failed()) return 0;
    mesh::MeshView const& v = r.value();
    for (u32 i = 0; i < v.textures().size(); ++i)
        (void)v.str(v.textures()[i].pathStr);
    for (u32 i = 0; i < v.lods().size(); ++i)
        for (u32 s = 0; s < mesh::kMaxStreams; ++s)
            (void)v.stream_bytes(v.lods()[i], s);
    if (v.encoded().empty() || v.decoded_size() > (u64(64) << 20)) return 0; // keep runs fast
    Vec<u8> decoded(default_allocator(), Tag::Test);
    decoded.resize(usize(v.decoded_size()));
    if (mesh::decode_payload(v, v.encoded(), decoded.span(), {.verifyChecksums = true}).ok())
        (void)mesh::check_indices(v, decoded.span());
    return 0;
}
