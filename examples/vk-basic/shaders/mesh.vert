#version 460
// examples/vk-basic/shaders/mesh.vert — float vertex data (VertexProfile::Float) at the viewer's
// fixed locations. Set 0 is the frame, set 1 the material (mesh.frag).

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent; // zeros when the mesh has none: mesh.frag keeps the normal
layout(location = 3) in vec2 inUv0;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 tonemap; // x: 2^exposure
} frame;

layout(push_constant) uniform Draw {
    mat4 model;
    vec4 posScale;
    vec4 posBias;
    uint unused;
    uint textures; // bit per set 1 binding that holds a texture
    uint pad0, pad1;
} draw;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUv;

void main() {
    vec4 world  = draw.model * vec4(inPosition, 1.0);
    mat3 m      = mat3(draw.model); // rotation and uniform scale only
    vWorld      = world.xyz;
    vNormal     = m * inNormal;
    vTangent    = vec4(m * inTangent.xyz, inTangent.w);
    vUv         = inUv0;
    gl_Position = frame.viewProj * world;
}
