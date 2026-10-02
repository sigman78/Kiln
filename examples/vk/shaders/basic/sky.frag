#version 460
// examples/vk/shaders/basic/sky.frag — the scene cube at set 2, binding 0, along the camera ray
// through this pixel. The target is sRGB: the output stays linear.

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    vec4 cameraPos;
    vec4 lightDir;
    vec4 tonemap; // x: 2^exposure
} frame;

layout(set = 2, binding = 0) uniform samplerCube uSky;

// The camera basis; right and up are scaled by the half extents of the view at distance 1.
layout(push_constant) uniform Sky {
    vec4 forward;
    vec4 right;
    vec4 up;
    uint unused;
} sky;

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

vec3 aces(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }

void main() {
    // NDC +y is down in Vulkan; cube maps use a left-handed frame (docs/design/texture-shapes.md).
    vec3 dir = normalize(sky.forward.xyz + vNdc.x * sky.right.xyz - vNdc.y * sky.up.xyz);
    outColor = vec4(aces(texture(uSky, vec3(dir.x, dir.y, -dir.z)).rgb * frame.tonemap.x), 1.0);
}
