#version 460
#extension GL_EXT_nonuniform_qualifier : require
// examples/viewer/shaders/sky.frag — the cube map in the bindless cube binding, seen along
// the camera ray through this pixel.

layout(set = 0, binding = 1) uniform samplerCube cubes[];

// The camera basis; right and up are scaled by the half extents of the view at distance 1.
layout(push_constant) uniform Sky {
    vec4 forward;
    vec4 right;
    vec4 up;
    uint cubeSlot;
} sky;

layout(location = 0) in vec2 vNdc;

layout(location = 0) out vec4 outColor;

void main() {
    // NDC +y is down in Vulkan.
    vec3 dir = sky.forward.xyz + vNdc.x * sky.right.xyz - vNdc.y * sky.up.xyz;
    // Cube maps use a left-handed frame (KTX2, Vulkan); the viewer's world is right-handed.
    outColor = vec4(texture(cubes[sky.cubeSlot], vec3(dir.x, dir.y, -dir.z)).rgb, 1.0);
}
