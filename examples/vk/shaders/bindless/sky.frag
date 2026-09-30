#version 460
#extension GL_EXT_nonuniform_qualifier : require
// examples/vk/shaders/bindless/sky.frag — the cube map in the bindless cube binding, seen along
// the camera ray through this pixel.

layout(set = 0, binding = 1) uniform samplerCube cubes[];

layout(set = 1, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 tonemap;
} frame;

// Display transform (frame.tonemap: x = exposure multiplier, y = mode). The target is an sRGB
// image, so the output stays linear and the hardware encodes it. Mode 1 is Narkowicz's ACES fit.
vec3 tonemap(vec3 c) {
    c *= frame.tonemap.x;
    if (frame.tonemap.y > 0.5) c = clamp((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
    return c;
}

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
    outColor = vec4(tonemap(texture(cubes[sky.cubeSlot], vec3(dir.x, dir.y, -dir.z)).rgb), 1.0);
}
