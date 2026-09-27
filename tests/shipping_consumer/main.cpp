// tests/shipping_consumer/main.cpp — read-only consumer of the shipping headers, no cook code.
// Opens a hand-built KTX2 image and a broken .mesh header. Exits 0 on success.
#include <kiln/containers.h>
#include <kiln/formats.h>
#include <kiln/hash.h>
#include <kiln/ktx2.h>
#include <kiln/log.h>
#include <kiln/mesh.h>
#include <kiln/result.h>

#include <cstdio>
#include <cstring>

using namespace kiln;
using namespace kiln::literals;

namespace {
int fails = 0;
void check(bool ok, char const* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++fails;
    }
}
} // namespace

int main() {
    // Core vocabulary.
    Vec<u32> v(default_allocator(), Tag::General);
    for (u32 i = 0; i < 8; ++i)
        v.push_back(i * i);
    HashMap<u64, int> m;
    m.insert("meshes/ship"_h, 7);
    check(v[7] == 49 && *m.find("meshes/ship"_h) == 7, "containers");

    // Format table.
    check(format_info(Format::R8G8B8A8_SRGB)->bytesPerBlock == 4, "format table");
    check(u32(Format::BC7_SRGB) == 146, "VkFormat numbering");

    // .mesh reader rejects a wrong magic with a diagnostic, no crash.
    alignas(8) u8 bad[sizeof(mesh::FileHeader)] = {};
    std::memcpy(bad, "NOPE", 4);
    Result<mesh::MeshView> r = mesh::MeshView::open(Span<u8 const>(bad, sizeof bad));
    check(r.failed() && r.code() == Code::Corrupt, "mesh reader rejects bad magic");

    // KTX2 reader on a hand-built 1x1 R8_UNORM image (header + 1 level index + 44-B DFD + data).
    alignas(8) u8 ktx[80 + 24 + 44 + 4] = {};
    std::memcpy(ktx, ktx2::kIdentifier, 12);
    auto put32 = [&](usize off, u32 x) { std::memcpy(ktx + off, &x, 4); };
    auto put64 = [&](usize off, u64 x) { std::memcpy(ktx + off, &x, 8); };
    put32(12, u32(Format::R8_UNORM));     // vkFormat
    put32(16, 1);                         // typeSize
    put32(20, 1);                         // width
    put32(24, 1);                         // height
    put32(28, 0);                         // depth
    put32(32, 0);                         // layers
    put32(36, 1);                         // faces
    put32(40, 1);                         // levels
    put32(44, 0);                         // supercompression
    put32(48, 104);                       // dfd offset
    put32(52, 44);                        // dfd length
    put32(56, 148);                       // kvd offset
    put32(60, 0);                         // kvd length
    put64(80, 148);                       // level 0 offset
    put64(88, 1);                         // byteLength
    put64(96, 1);                         // uncompressedByteLength
    put32(104, 44);                       // DFD totalSize
    put32(108, 0);                        // vendor/type
    put32(112, u32(2) | (u32(40) << 16)); // version 2, block size 40
    ktx[116]                 = 1;         // colorModel RGBSDA
    ktx[117]                 = 1;         // primaries
    ktx[118]                 = 1;         // transfer linear
    ktx[124]                 = 1;         // bytesPlane0
    ktx[148]                 = 0x7f;      // the pixel
    Result<ktx2::Ktx2View> k = ktx2::Ktx2View::open(Span<u8 const>(ktx, sizeof ktx));
    check(k.ok(), "ktx2 reader opens hand-built image");
    if (k.ok()) {
        check(k->desc().format == Format::R8_UNORM && k->desc().width == 1, "ktx2 desc");
        check(k->level_data(0).size == 1 && k->level_data(0)[0] == 0x7f, "ktx2 level data");
    }

    KILN_INFO("consumer", "shipping consumer: %d failure(s)", fails);
    return fails ? 1 : 0;
}
