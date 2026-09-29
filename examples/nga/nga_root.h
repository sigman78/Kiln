// examples/nga/nga_root.h — the root data of scene.slang and sky.slang, as C++ writes it (C layout;
// matrices column-major). The offsets are the ones Slang reflects for those shaders.
#pragma once

#include <kiln/core.h>

#include <cstddef>

namespace kiln::nga {

inline constexpr u32 kNoTexture = 0xFFFFFFFFu;

struct MeshRoot {
    f32 model[16];
    f32 viewProj[16];
    f32 eyeExposure[4]; ///< xyz: camera; w: 2^exposure
    u64 stream0;        ///< GPU address of the positions (float3 per vertex)
    u64 stream1;        ///< GPU address of normal, tangent, UV 0
    u32 stride0;        ///< in floats
    u32 stride1;
    u32 normalOffset;  ///< in floats, within stream 1
    u32 tangentOffset; ///< kNoTexture: none
    u32 uvOffset;      ///< kNoTexture: none
    u32 vertexBase;    ///< added to SV_VertexID
    u32 texBaseColor;  ///< descriptor indices, kNoTexture where the material has none
    u32 texNormal;
    u32 texMetalRough;
    u32 texOcclusion;
    u32 texEmissive;
    u32 texSky;
};
static_assert(offsetof(MeshRoot, stream0) == 144 && offsetof(MeshRoot, stride0) == 160);
static_assert(offsetof(MeshRoot, texBaseColor) == 184 && sizeof(MeshRoot) == 208);

struct SkyRoot {
    f32 forward[4]; ///< xyz; w: 2^exposure
    f32 right[4];   ///< scaled by tan(fov / 2) * aspect
    f32 up[4];      ///< scaled by tan(fov / 2)
    u32 sky;        ///< descriptor index of the cube
    u32 pad[3];
};
static_assert(sizeof(SkyRoot) == 64);

} // namespace kiln::nga
