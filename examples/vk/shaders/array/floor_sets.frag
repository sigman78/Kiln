#version 460
#extension GL_GOOGLE_include_directive : require
// examples/vk/shaders/array/floor_sets.frag — the array through the host's own descriptor set
// (set 1, binding 0), rewritten when kiln's events say its GPU object changed.

layout(set = 1, binding = 0) uniform sampler2DArray uTiles;

vec4 sample_tile(vec2 uv, float layer) { return texture(uTiles, vec3(uv, layer)); }

#include "floor.glsl"
