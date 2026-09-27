#version 460
// examples/viewer/shaders/mesh.vert — spec §8 model A: attributes at fixed locations by
// semantic; specialization constants say which are real and whether the normal is octahedral.

layout(constant_id = 0) const bool kNormalOct  = true;  // R16G16_SNORM octahedral vs xyz
layout(constant_id = 1) const bool kHasTangent = false;
layout(constant_id = 2) const bool kHasUv0     = false;
layout(constant_id = 3) const bool kHasColor   = false;

layout(location = 0) in vec4 inPosition; // quantized: unorm xyz(w) * posScale + posBias; float: xyz
layout(location = 1) in vec4 inNormal;   // oct: xy in [-1,1]; raw: xyz
layout(location = 2) in vec4 inTangent;  // xyz + sign
layout(location = 3) in vec2 inUv0;
layout(location = 4) in vec2 inUv1;
layout(location = 5) in vec4 inColor;

layout(set = 1, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir; // xyz, w unused
} frame;

layout(push_constant) uniform Draw {
    mat4 model;
    vec4 posScale; // xyz, w unused
    vec4 posBias;  // xyz, w unused
    uint baseColorSlot;
    uint flags;    // bit 0: baseColorSlot valid; bit 1: vertex color
    uint pad0, pad1;
} draw;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec4 vColor;
layout(location = 3) out vec3 vWorldPos;

vec3 oct_decode(vec2 f) {
    vec3 n = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

void main() {
    vec3 local = inPosition.xyz * draw.posScale.xyz + draw.posBias.xyz;
    vec4 world = draw.model * vec4(local, 1.0);
    vec3 n     = kNormalOct ? oct_decode(inNormal.xy) : normalize(inNormal.xyz);
    vNormal    = normalize(mat3(draw.model) * n);
    vUv        = kHasUv0 ? inUv0 : vec2(0.0);
    vColor     = kHasColor ? inColor : vec4(1.0);
    vWorldPos  = world.xyz;
    gl_Position = frame.viewProj * world;
}
