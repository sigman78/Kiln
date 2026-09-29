// examples/sokol/readback.cpp — reads a color attachment back for --dump. sokol_gfx has no
// readback, so this reaches into the backend: D3D11 on Windows, GL elsewhere; not Metal.
#include "readback.h"

#include <kiln/log.h>

#if defined(SOKOL_D3D11)
#include <d3d11.h>
#elif defined(SOKOL_GLCORE)
#include <GL/gl.h>
#endif

namespace kiln::sk {

bool read_rgba(sg_image image, u32 width, u32 height, Vec<u8>& out) noexcept {
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

} // namespace kiln::sk
