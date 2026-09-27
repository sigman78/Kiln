#version 460
#extension GL_EXT_nonuniform_qualifier : require
// examples/viewer/shaders/mesh.frag — bindless base color (slot from the adapter) with a
// simple directional light. Placeholders arrive through the same slots, so nothing here
// knows whether the real texture has landed.

layout(set = 0, binding = 0) uniform sampler2D textures[];

layout(set = 1, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
} frame;

layout(push_constant) uniform Draw {
    mat4 model;
    vec4 posScale;
    vec4 posBias;
    uint baseColorSlot;
    uint flags;
    uint pad0, pad1;
} draw;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vColor;
layout(location = 3) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 base = vColor;
    if ((draw.flags & 1u) != 0u) base *= texture(textures[nonuniformEXT(draw.baseColorSlot)], vUv);
    vec3 n      = normalize(vNormal);
    vec3 l      = normalize(frame.lightDir.xyz);
    float ndl   = max(dot(n, l), 0.0);
    vec3 lit    = base.rgb * (0.15 + 0.85 * ndl);
    outColor    = vec4(lit, base.a);
}
