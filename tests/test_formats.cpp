#include "kiln_test.h"

#include "kiln/formats.h"

using namespace kiln;

static_assert(is_known_format(Format::R8G8B8A8_SRGB));
static_assert(u32(Format::R8G8B8_SRGB) == 29 && format_info(Format::R8G8B8_SRGB)->bytesPerBlock == 3);
static_assert(u32(Format::ETC2_R8G8B8A8_SRGB) == 152 &&
              format_info(Format::ETC2_R8G8B8A8_SRGB)->bytesPerBlock == 16);
static_assert(u32(Format::ASTC_4x4_UNORM) == 157 && u32(Format::ASTC_12x12_SRGB) == 184);
static_assert(format_info(Format::ASTC_8x8_UNORM)->blockWidth == 8 &&
              format_image_bytes(Format::ASTC_8x8_UNORM, 9, 9) == 64);
static_assert(linear_format(Format::ETC2_R8G8B8_SRGB) == Format::ETC2_R8G8B8_UNORM);
static_assert(format_info(Format::R8G8B8A8_SRGB)->bytesPerBlock == 4);
static_assert(linear_format(Format::R8G8B8A8_SRGB) == Format::R8G8B8A8_UNORM);
static_assert(linear_format(Format::R8G8B8A8_UNORM) == Format::R8G8B8A8_UNORM);
static_assert(srgb_format(Format::BC7_UNORM) == Format::BC7_SRGB);
static_assert(srgb_format(Format::R16G16_SFLOAT) == Format::R16G16_SFLOAT);
static_assert(format_image_bytes(Format::BC1_RGB_UNORM, 5, 5) == 32u); // 2x2 blocks of 8 B
static_assert(format_image_bytes(Format::R16G16_SFLOAT, 3, 2) == 24u);
static_assert(format_row_bytes(Format::BC7_SRGB, 9) == 48u); // 3 blocks of 16 B
static_assert(!is_known_format(Format(999)));
static_assert(format_image_bytes(Format(999), 4, 4) == 0u);
static_assert(StrView(format_name(Format::Undefined)) == "UNDEFINED"_sv);
static_assert(StrView(format_name(Format::R32G32B32_SFLOAT)) == "R32G32B32_SFLOAT"_sv);
static_assert(u32(Format::R8G8B8A8_SRGB) == 43u); // VkFormat values
static_assert(u32(Format::BC7_SRGB) == 146u);
static_assert(is_srgb_format(Format::BC1_RGBA_SRGB) && !is_srgb_format(Format::BC1_RGBA_UNORM));
static_assert(is_compressed_format(Format::BC5_UNORM) && !is_compressed_format(Format::R8G8_UNORM));
static_assert(format_block_bytes(Format::R32G32B32A32_SFLOAT) == 16u);

KILN_TEST(Formats, TableLookupIsIdentity) {
    for (usize i = 0; i < kFormatCount; ++i) {
        FormatInfo const& e = detail::kFormatTable[i];
        KILN_CHECK_MSG(format_info(e.format) == &e, "entry %u", unsigned(i));
        KILN_CHECK(is_known_format(e.format));
        KILN_CHECK(StrView(format_name(e.format)) == StrView(e.name));
        KILN_CHECK(e.bytesPerBlock > 0);
    }
    KILN_CHECK(format_info(Format::Undefined) == nullptr);
}
