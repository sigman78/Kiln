#version 460
// examples/vk/shaders/basic/mesh.frag — the material's textures from set 1, one descriptor set per
// material, lit like the other integration examples. The target is sRGB: the output stays linear.

struct Material {
    vec4 baseColor;
    vec4 emissiveNormal; // xyz: emissive factor; w: normal scale
    vec4 mro;            // metallic, roughness, occlusion strength
};
layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 tonemap; // x: 2^exposure
    Material materials[128]; // vkx::kMaxMaterials
} frame;

layout(set = 1, binding = 0) uniform sampler2D uBaseColor;
layout(set = 1, binding = 1) uniform sampler2D uNormalMap;
layout(set = 1, binding = 2) uniform sampler2D uMetalRough;
layout(set = 1, binding = 3) uniform sampler2D uOcclusion;
layout(set = 1, binding = 4) uniform sampler2D uEmissive;
layout(set = 2, binding = 0) uniform samplerCube uSky;

layout(push_constant) uniform Draw {
    mat4 model;
    vec4 posScale;
    vec4 posBias;
    uint unused;
    uint textures; // bits 0..4: material textures; bit 5: scene environment
    uint material; // into frame.materials
    uint pad1;
} draw;

layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv;
layout(location = 0) out vec4 outColor;

vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
// Cube maps use a left-handed frame; this world is right-handed (docs/design/texture-shapes.md).
vec3 cube_dir(vec3 d) { return vec3(d.x, d.y, -d.z); }
bool has(uint binding) { return (draw.textures & (1u << binding)) != 0u; }

// Z is rebuilt from X and Y, so a BC5 normal map (X and Y only) and an RGBA8 one both work;
// `scale` then scales X and Y.
vec3 perturb(vec3 n, vec4 tangent, vec2 texel, float scale) {
    if (dot(tangent.xyz, tangent.xyz) == 0.0) return n;
    vec3 t  = normalize(tangent.xyz - n * dot(n, tangent.xyz));
    vec3 b  = cross(n, t) * tangent.w;
    vec2 xy = texel * 2.0 - 1.0;
    return normalize(mat3(t, b, n) * vec3(xy * scale, sqrt(max(1.0 - dot(xy, xy), 0.0))));
}

vec3 shade(vec3 base, vec3 n, vec3 v, float rough, float metal, float ao, vec3 ambient, vec3 env) {
    vec3 l       = normalize(vec3(0.45, 0.8, 0.6));
    vec3 h       = normalize(l + v);
    vec3 f0      = mix(vec3(0.04), base, metal);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    float ndl    = max(dot(n, l), 0.0);
    float shine  = mix(512.0, 4.0, rough);
    vec3 spec    = fresnel * pow(max(dot(n, h), 0.0), shine) * (shine + 8.0) / 25.13 * ndl;
    return (base * (1.0 - metal) * (ndl * 2.0 + ambient) + spec * 2.0 + env * fresnel * (1.0 - rough)) * ao;
}

void main() {
    Material m    = frame.materials[draw.material];
    vec3 base     = m.baseColor.rgb * (has(0u) ? texture(uBaseColor, vUv).rgb : vec3(1.0));
    vec3 n        = normalize(vNormal);
    if (has(1u)) n = perturb(n, vTangent, texture(uNormalMap, vUv).xy, m.emissiveNormal.w);
    vec3 mr       = has(2u) ? texture(uMetalRough, vUv).rgb : vec3(1.0); // G rough, B metal
    float ao      = has(3u) ? 1.0 + m.mro.z * (texture(uOcclusion, vUv).r - 1.0) : 1.0;
    vec3 emissive = m.emissiveNormal.rgb * (has(4u) ? texture(uEmissive, vUv).rgb : vec3(1.0));
    float rough   = clamp(m.mro.y * mr.g, 0.05, 1.0);
    mr.b *= m.mro.x;
    vec3 v        = normalize(frame.cameraPos.xyz - vWorld);
    vec3 ambient  = has(5u) ? textureLod(uSky, cube_dir(n), 6.0).rgb : vec3(0.3);
    vec3 env      = has(5u) ? textureLod(uSky, cube_dir(reflect(-v, n)), rough * 6.0).rgb : vec3(0.3);
    vec3 color    = shade(base, n, v, rough, mr.b, ao, ambient, env) + emissive;
    outColor      = vec4(aces(color * frame.tonemap.x), 1.0);
}
