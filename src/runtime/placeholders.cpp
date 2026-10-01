// placeholders.cpp — static pixels for the built-in placeholders and the Failed checker.
#include "kiln/placeholders.h"

#include <utility> // std::unreachable

namespace kiln {

namespace {

constexpr u8 kBaseColorPixel[4] = {128, 128, 128, 255}; // mid-grey, sRGB
constexpr u8 kNormalPixel[4]    = {128, 128, 255, 255}; // flat normal, linear
constexpr u8 kOrmPixel[4]       = {255, 255, 0, 255};   // AO 1, roughness 1, metallic 0, linear
constexpr u8 kEmissivePixel[4]  = {0, 0, 0, 255};       // black, sRGB

inline constexpr u32 kFailedExtent = 8;
inline constexpr u32 kFailedCell   = 2;

// 8x8 magenta/black checker, 2x2 cells, sRGB.
struct FailedChecker {
    u8 pixels[kFailedExtent * kFailedExtent * 4]{};

    constexpr FailedChecker() {
        for (u32 y = 0; y < kFailedExtent; ++y) {
            for (u32 x = 0; x < kFailedExtent; ++x) {
                bool const magenta = ((x / kFailedCell) + (y / kFailedCell)) % 2 == 0;
                u8* const p        = &pixels[(y * kFailedExtent + x) * 4];
                p[0]               = magenta ? 255 : 0;
                p[1]               = 0;
                p[2]               = magenta ? 255 : 0;
                p[3]               = 255;
            }
        }
    }
};
constexpr FailedChecker kFailedChecker{};

} // namespace

PlaceholderImage builtin_placeholder(TextureKind kind) {
    switch (kind) {
    case TextureKind::BaseColor: return {Format::R8G8B8A8_SRGB, 1, 1, Span<u8 const>(kBaseColorPixel)};
    case TextureKind::Normal: return {Format::R8G8B8A8_UNORM, 1, 1, Span<u8 const>(kNormalPixel)};
    case TextureKind::Orm: return {Format::R8G8B8A8_UNORM, 1, 1, Span<u8 const>(kOrmPixel)};
    case TextureKind::Emissive: return {Format::R8G8B8A8_SRGB, 1, 1, Span<u8 const>(kEmissivePixel)};
    case TextureKind::Count: break;
    }
    KILN_ASSERT(false && "builtin_placeholder: invalid TextureKind");
    std::unreachable();
}

PlaceholderImage builtin_failed_placeholder() {
    return {Format::R8G8B8A8_SRGB, kFailedExtent, kFailedExtent, Span<u8 const>(kFailedChecker.pixels)};
}

} // namespace kiln
