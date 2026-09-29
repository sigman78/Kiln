// examples/nga/sky.slang — the cube behind the model, along the camera ray through each pixel.
// SkyRoot matches nga_root.h.
#include <NoGraphicsAPI/shader.slang>

struct SkyRoot
{
    float4 forward; // xyz; w: 2^exposure
    float4 right;   // scaled by tan(fov / 2) * aspect
    float4 up;      // scaled by tan(fov / 2)
    uint sky;       // descriptor index of the cube
    uint pad0;
    uint pad1;
    uint pad2;
};

GPU_ROOT(SkyRoot, root);

float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }

struct SkyVaryings
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

[shader("vertex")]
SkyVaryings skyVertex(uint vertex_id : SV_VertexID)
{
    SkyVaryings o;
    o.ndc      = float2((vertex_id << 1) & 2, vertex_id & 2) * 2.0 - 1.0; // one triangle over the screen
    o.position = float4(o.ndc, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 skyFragment(SkyVaryings i) : SV_Target0
{
    // Vulkan NDC +y is down; cube maps use a left-handed frame (docs/design/texture-shapes.md).
    float3 d = normalize(root.forward.xyz + i.ndc.x * root.right.xyz - i.ndc.y * root.up.xyz);
    float3 c = gpu_texture<TextureCube<float4>>(root.sky).Sample(gpu_sampler(1), float3(d.x, d.y, -d.z)).rgb;
    return float4(aces(c * root.forward.w), 1.0); // the target is sRGB: output stays linear
}
