// examples/nga/scene.slang — kiln-nga's model shaders: vertices are pulled through GPU pointers into
// kiln's mesh payload, textures are descriptor heap indices; lit like the other integration
// examples. MeshRoot matches nga_root.h (C layout, column-major matrices).
#include <NoGraphicsAPI/shader.slang>

struct MeshRoot
{
    float4x4 model;
    float4x4 view_proj;
    float4 eye_exposure;   // xyz: camera; w: 2^exposure
    float* stream0;        // positions: float3 per vertex at stride0
    float* stream1;        // normal, tangent, UV 0 at the offsets below, stride1
    uint stride0;          // in floats
    uint stride1;
    uint normal_offset;    // in floats, within stream 1
    uint tangent_offset;   // 0xFFFFFFFF: none
    uint uv_offset;        // 0xFFFFFFFF: none
    uint vertex_base;      // added to SV_VertexID; draws pass vertex_offset 0
    // Descriptor indices, 0xFFFFFFFF where the material has none.
    uint tex_base_color;
    uint tex_normal;
    uint tex_metal_rough;
    uint tex_occlusion;
    uint tex_emissive;
    uint tex_sky;
    float4 base_color_factor;
    float4 emissive_normal; // xyz: emissive factor; w: normal scale
    float4 mro;             // metallic, roughness, occlusion strength
};

static const uint kNone = 0xFFFFFFFFu;

float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
// Cube maps use a left-handed frame; this world is right-handed (docs/design/texture-shapes.md).
float3 cube_dir(float3 d) { return float3(d.x, d.y, -d.z); }

// Z is rebuilt from X and Y, so a BC5 normal map (X and Y only) and an RGBA8 one both work; `scale`
// then scales X and Y.
float3 perturb(float3 n, float4 tangent, float2 texel, float scale)
{
    if (dot(tangent.xyz, tangent.xyz) == 0.0) return n;
    float3 t  = normalize(tangent.xyz - n * dot(n, tangent.xyz));
    float3 b  = cross(n, t) * tangent.w;
    float2 xy = texel * 2.0 - 1.0;
    return normalize((t * xy.x + b * xy.y) * scale + n * sqrt(max(1.0 - dot(xy, xy), 0.0)));
}

float3 shade(float3 base, float3 n, float3 v, float rough, float metal, float ao, float3 ambient, float3 env)
{
    float3 l       = normalize(float3(0.45, 0.8, 0.6));
    float3 h       = normalize(l + v);
    float3 f0      = lerp(float3(0.04), base, metal);
    float3 fresnel = f0 + (1.0 - f0) * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    float ndl      = max(dot(n, l), 0.0);
    float shine    = lerp(512.0, 4.0, rough);
    float3 spec    = fresnel * pow(max(dot(n, h), 0.0), shine) * (shine + 8.0) / 25.13 * ndl;
    return (base * (1.0 - metal) * (ndl * 2.0 + ambient) + spec * 2.0 + env * fresnel * (1.0 - rough)) * ao;
}

// --- Mesh ---------------------------------------------------------------------------------------

GPU_ROOT(MeshRoot, mesh);

struct MeshVaryings
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 tangent : TEXCOORD2;
    float2 uv : TEXCOORD3;
};

float3 load3(float* p) { return float3(p[0], p[1], p[2]); }

[shader("vertex")]
MeshVaryings meshVertex(uint vertex_id : SV_VertexID)
{
    uint v          = vertex_id + mesh.vertex_base;
    float* attribs  = mesh.stream1 + v * mesh.stride1;
    float3 position = load3(mesh.stream0 + v * mesh.stride0);
    float3 normal   = load3(attribs + mesh.normal_offset);
    float4 tangent  = mesh.tangent_offset == kNone ? float4(0.0) : float4(load3(attribs + mesh.tangent_offset), attribs[mesh.tangent_offset + 3]);
    float2 uv       = mesh.uv_offset == kNone ? float2(0.0) : float2(attribs[mesh.uv_offset], attribs[mesh.uv_offset + 1]);

    float4 world = mul(mesh.model, float4(position, 1.0));
    float3x3 m   = float3x3(mesh.model[0].xyz, mesh.model[1].xyz, mesh.model[2].xyz); // rotation, uniform scale
    MeshVaryings o;
    o.position = mul(mesh.view_proj, world);
    o.world    = world.xyz;
    o.normal   = mul(m, normal);
    o.tangent  = float4(mul(m, tangent.xyz), tangent.w);
    o.uv       = uv;
    return o;
}

float4 tex(uint index, float2 uv)
{
    return gpu_texture<Texture2D<float4>>(index).Sample(gpu_sampler(0), uv); // sampler 0: repeat
}

float3 sky_lod(float3 d, float lod)
{
    return gpu_texture<TextureCube<float4>>(mesh.tex_sky).SampleLevel(gpu_sampler(1), cube_dir(d), lod).rgb;
}

[shader("fragment")]
float4 meshFragment(MeshVaryings i) : SV_Target0
{
    float3 base     = mesh.base_color_factor.rgb * (mesh.tex_base_color != kNone ? tex(mesh.tex_base_color, i.uv).rgb : float3(1.0));
    float3 n        = normalize(i.normal);
    if (mesh.tex_normal != kNone) n = perturb(n, i.tangent, tex(mesh.tex_normal, i.uv).xy, mesh.emissive_normal.w);
    float3 mr       = mesh.tex_metal_rough != kNone ? tex(mesh.tex_metal_rough, i.uv).rgb : float3(1.0);
    float ao        = mesh.tex_occlusion != kNone ? 1.0 + mesh.mro.z * (tex(mesh.tex_occlusion, i.uv).r - 1.0) : 1.0;
    float3 emissive = mesh.emissive_normal.rgb * (mesh.tex_emissive != kNone ? tex(mesh.tex_emissive, i.uv).rgb : float3(1.0));
    float rough     = clamp(mesh.mro.y * mr.g, 0.05, 1.0); // G rough, B metal
    mr.b *= mesh.mro.x;
    float3 v        = normalize(mesh.eye_exposure.xyz - i.world);
    bool sky        = mesh.tex_sky != kNone;
    float3 ambient  = sky ? sky_lod(n, 6.0) : float3(0.3);
    float3 env      = sky ? sky_lod(reflect(-v, n), rough * 6.0) : float3(0.3);
    float3 color    = shade(base, n, v, rough, mr.b, ao, ambient, env) + emissive;
    return float4(aces(color * mesh.eye_exposure.w), 1.0); // the target is sRGB: output stays linear
}
