// examples/sokol/readback.cpp — reads a color attachment back for --dump and a texture for --verify.
// sokol_gfx has no readback, so this reaches into the backend: D3D11 on Windows, GL elsewhere; not Metal.
#include "readback.h"

#include <kiln/formats.h>
#include <kiln/log.h>

#include <cstring>

#if defined(SOKOL_D3D11)
#include <d3d11.h>
#elif defined(SOKOL_GLCORE)
#include <GL/gl.h>
#endif

namespace kiln::sk {

bool read_rgba(sg_image image, u32 width, u32 height, Vec<u8>& out) {
    out.resize(usize(width) * height * 4);
#if defined(SOKOL_D3D11)
    auto* device = static_cast<ID3D11Device*>(const_cast<void*>(sg_d3d11_device()));
    auto* dc     = static_cast<ID3D11DeviceContext*>(const_cast<void*>(sg_d3d11_device_context()));
    auto* tex    = static_cast<ID3D11Texture2D*>(const_cast<void*>(sg_d3d11_query_image_info(image).tex2d));
    if (!device || !dc || !tex) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    td.Usage                 = D3D11_USAGE_STAGING;
    td.BindFlags             = 0;
    td.CPUAccessFlags        = D3D11_CPU_ACCESS_READ;
    td.MiscFlags             = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) return false;
    dc->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    bool const ok = SUCCEEDED(dc->Map(staging, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        bool const bgra =
            td.Format == DXGI_FORMAT_B8G8R8A8_UNORM || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for (u32 y = 0; y < height; ++y) {
            u8 const* src = static_cast<u8 const*>(m.pData) + usize(y) * m.RowPitch;
            u8* dst       = out.data() + usize(y) * width * 4;
            for (u32 x = 0; x < width; ++x) {
                dst[x * 4 + 0] = src[x * 4 + (bgra ? 2 : 0)];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + (bgra ? 0 : 2)];
                dst[x * 4 + 3] = src[x * 4 + 3];
            }
        }
        dc->Unmap(staging, 0);
    }
    staging->Release();
    return ok;
#elif defined(SOKOL_GLCORE)
    sg_gl_image_info const info = sg_gl_query_image_info(image);
    glBindTexture(GL_TEXTURE_2D, info.tex[info.active_slot]);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    usize const row = usize(width) * 4; // GL rows run bottom-up
    for (u32 y = 0; y < height / 2; ++y)
        for (usize x = 0; x < row; ++x) {
            u8 const t                      = out[y * row + x];
            out[y * row + x]                = out[(height - 1 - y) * row + x];
            out[(height - 1 - y) * row + x] = t;
        }
    return true;
#else
    (void)image;
    KILN_ERROR("sokol", "--dump has no readback for this backend");
    return false;
#endif
}

bool read_texture(sg_image image, TextureDesc const& desc, Vec<u8>& out) {
    u64 total = 0;
    for (u32 i = 0; i < desc.levels; ++i)
        total += format_image_bytes(desc.format, max(desc.width >> i, 1u), max(desc.height >> i, 1u)) *
                 desc.layers;
    out.resize(usize(total));
#if defined(SOKOL_D3D11)
    auto* device = static_cast<ID3D11Device*>(const_cast<void*>(sg_d3d11_device()));
    auto* dc     = static_cast<ID3D11DeviceContext*>(const_cast<void*>(sg_d3d11_device_context()));
    auto* tex    = static_cast<ID3D11Texture2D*>(const_cast<void*>(sg_d3d11_query_image_info(image).tex2d));
    if (!device || !dc || !tex) return false;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    if (td.MipLevels != desc.levels || td.ArraySize != desc.layers) return false;
    td.Usage                 = D3D11_USAGE_STAGING;
    td.BindFlags             = 0;
    td.CPUAccessFlags        = D3D11_CPU_ACCESS_READ;
    td.MiscFlags             = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) return false;
    dc->CopyResource(staging, tex);
    bool ok = true;
    u64 off = 0;
    for (u32 level = 0; level < desc.levels && ok; ++level) {
        u32 const w        = max(desc.width >> level, 1u);
        u64 const rowBytes = format_row_bytes(desc.format, w);
        u64 const bytes    = format_image_bytes(desc.format, w, max(desc.height >> level, 1u));
        for (u32 layer = 0; layer < desc.layers && ok; ++layer) {
            D3D11_MAPPED_SUBRESOURCE m{};
            UINT const sub = D3D11CalcSubresource(level, layer, td.MipLevels);
            ok             = SUCCEEDED(dc->Map(staging, sub, D3D11_MAP_READ, 0, &m));
            if (!ok) break;
            for (u64 r = 0; r < bytes / rowBytes; ++r) // block rows
                std::memcpy(out.data() + off + r * rowBytes, static_cast<u8 const*>(m.pData) + r * m.RowPitch,
                            usize(rowBytes));
            dc->Unmap(staging, sub);
            off += bytes;
        }
    }
    staging->Release();
    return ok;
#elif defined(SOKOL_GLCORE) && defined(GL_TEXTURE_2D_ARRAY)
    if (!is_compressed_format(desc.format)) return false;
    sg_gl_image_info const info = sg_gl_query_image_info(image);
    GLenum const target         = desc.layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    glBindTexture(target, info.tex[info.active_slot]);
    u64 off = 0;
    for (u32 level = 0; level < desc.levels; ++level) {
        glGetCompressedTexImage(target, GLint(level), out.data() + off);
        off += format_image_bytes(desc.format, max(desc.width >> level, 1u), max(desc.height >> level, 1u)) *
               desc.layers;
    }
    glBindTexture(target, 0);
    return glGetError() == GL_NO_ERROR;
#else
    (void)image;
    KILN_ERROR("sokol", "--verify has no texture readback for this backend");
    return false;
#endif
}

} // namespace kiln::sk
