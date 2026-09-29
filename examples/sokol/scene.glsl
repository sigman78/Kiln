// examples/sokol/scene.glsl — sokol-shdc input: the model and the sky, lit like the GL examples.
// One GL-style projection serves every backend: fixup_clipspace maps depth for D3D and Metal.

@block common
vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
vec3 to_srgb(vec3 c) { return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
vec4 display(vec3 color, float exposure) { return vec4(to_srgb(aces(color * exp2(exposure))), 1.0); }
// Cube maps use a left-handed frame; this world is right-handed (docs/design/texture-shapes.md).
vec3 cube_dir(vec3 d) { return vec3(d.x, d.y, -d.z); }
// A tangent-space normal map texel applied to n; without a tangent n stays. Z is rebuilt from X
// and Y, so a BC5 normal map (X and Y only) and an RGBA8 one both work; `scale` then scales X and Y.
vec3 perturb(vec3 n, vec4 tangent, vec2 texel, float scale) {
    if (dot(tangent.xyz, tangent.xyz) == 0.0) return n;
    vec3 t  = normalize(tangent.xyz - n * dot(n, tangent.xyz));
    vec3 b  = cross(n, t) * tangent.w;
    vec2 xy = texel * 2.0 - 1.0;
    return normalize(mat3(t, b, n) * vec3(xy * scale, sqrt(max(1.0 - dot(xy, xy), 0.0))));
}
// A sun plus the sky: `ambient` is the sky around n, `env` the sky in the reflected direction.
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
@end

@vs mesh_vs
@hlsl_options fixup_clipspace
@msl_options fixup_clipspace
layout(binding = 0) uniform mesh_vs_params {
    mat4 model;
    mat4 view_proj;
};
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 tangent;
layout(location = 3) in vec2 uv;
out vec3 v_world;
out vec3 v_normal;
out vec4 v_tangent;
out vec2 v_uv;
void main() {
    vec4 world  = model * vec4(pos, 1.0);
    mat3 m      = mat3(model); // rotation and uniform scale only
    v_world     = world.xyz;
    v_normal    = m * normal;
    v_tangent   = vec4(m * tangent.xyz, tangent.w);
    v_uv        = uv;
    gl_Position = view_proj * world;
}
@end

@fs mesh_fs
@include_block common
layout(binding = 1) uniform mesh_fs_params {
    vec4 eye;   // xyz: camera; w: exposure
    vec4 has_a; // 1 where a texture is bound: base color, normal, metal-rough, occlusion
    vec4 has_b; // emissive, sky
    vec4 base_color_factor;
    vec4 emissive_normal; // xyz: emissive factor; w: normal scale
    vec4 mro;             // metallic, roughness, occlusion strength
};
layout(binding = 0) uniform texture2D base_tex;
layout(binding = 1) uniform texture2D normal_tex;
layout(binding = 2) uniform texture2D mr_tex;
layout(binding = 3) uniform texture2D occlusion_tex;
layout(binding = 4) uniform texture2D emissive_tex;
layout(binding = 5) uniform textureCube sky_tex;
layout(binding = 0) uniform sampler smp;
layout(binding = 1) uniform sampler sky_smp;
in vec3 v_world;
in vec3 v_normal;
in vec4 v_tangent;
in vec2 v_uv;
out vec4 frag_color;
void main() {
    vec3 base     = base_color_factor.rgb * (has_a.x > 0.5 ? texture(sampler2D(base_tex, smp), v_uv).rgb : vec3(1.0));
    vec3 n        = normalize(v_normal);
    if (has_a.y > 0.5) n = perturb(n, v_tangent, texture(sampler2D(normal_tex, smp), v_uv).xy, emissive_normal.w);
    vec3 mr       = has_a.z > 0.5 ? texture(sampler2D(mr_tex, smp), v_uv).rgb : vec3(1.0); // G rough, B metal
    float ao      = has_a.w > 0.5 ? 1.0 + mro.z * (texture(sampler2D(occlusion_tex, smp), v_uv).r - 1.0) : 1.0;
    vec3 emissive = emissive_normal.rgb * (has_b.x > 0.5 ? texture(sampler2D(emissive_tex, smp), v_uv).rgb : vec3(1.0));
    float rough   = clamp(mro.y * mr.g, 0.05, 1.0);
    mr.b *= mro.x;
    vec3 v        = normalize(eye.xyz - v_world);
    bool sky      = has_b.y > 0.5;
    vec3 ambient  = sky ? textureLod(samplerCube(sky_tex, sky_smp), cube_dir(n), 6.0).rgb : vec3(0.3);
    vec3 env      = sky ? textureLod(samplerCube(sky_tex, sky_smp), cube_dir(reflect(-v, n)), rough * 6.0).rgb : vec3(0.3);
    frag_color    = display(shade(base, n, v, rough, mr.b, ao, ambient, env) + emissive, eye.w);
}
@end

@program mesh mesh_vs mesh_fs

@vs sky_vs
@hlsl_options fixup_clipspace
@msl_options fixup_clipspace
out vec2 v_ndc;
void main() { // one triangle that covers the screen
    v_ndc       = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 1.0, 1.0);
}
@end

@fs sky_fs
@include_block common
layout(binding = 0) uniform sky_fs_params {
    vec4 forward; // xyz; w: exposure
    vec4 right;   // scaled by tan(fov / 2) * aspect
    vec4 up;      // scaled by tan(fov / 2)
};
layout(binding = 5) uniform textureCube sky_tex;
layout(binding = 1) uniform sampler sky_smp;
in vec2 v_ndc;
out vec4 frag_color;
void main() {
    vec3 dir   = normalize(forward.xyz + v_ndc.x * right.xyz + v_ndc.y * up.xyz);
    frag_color = display(texture(samplerCube(sky_tex, sky_smp), cube_dir(dir)).rgb, forward.w);
}
@end

@program sky sky_vs sky_fs
