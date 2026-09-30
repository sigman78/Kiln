#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require
// examples/vk/shaders/array/floor_bindless.frag — the array through the adapter's bindless set:
// binding 2 holds the array textures, and the array's kiln slot never changes.

layout(set = 0, binding = 2) uniform sampler2DArray arrays[];

vec4 sample_tile(vec2 uv, float layer);

#include "floor.glsl"

vec4 sample_tile(vec2 uv, float layer) { return texture(arrays[nonuniformEXT(floorPush.slot)], vec3(uv, layer)); }
