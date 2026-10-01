// examples/sokol/array.glsl — sokol-shdc input for kiln-sokol-array: a floor of tiles, each a layer
// of one texture array; the same layout and "terrain" as kiln-gl-array and kiln-vk-array.

@vs floor_vs
@hlsl_options fixup_clipspace
@msl_options fixup_clipspace
out vec2 v_uv; // 0..1, top left first
void main() { // one triangle that covers the screen
    vec2 uv     = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv        = vec2(uv.x, 1.0 - uv.y);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs floor_fs
layout(binding = 0) uniform floor_params {
    vec4 grid; // xy: columns, rows; z: the layers the bound texture has (the placeholder has one)
};
layout(binding = 0) uniform texture2DArray tiles;
layout(binding = 0) uniform sampler smp;
in vec2 v_uv;
out vec4 frag_color;
vec3 to_srgb(vec3 c) { return mix(12.92 * c, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }
void main() {
    vec2 p    = (v_uv - 0.025) / 0.95 * grid.xy; // the floor covers 95% of the screen
    vec2 cell = floor(p);
    vec2 t    = fract(p) / 0.96;                 // a thin gap between tiles
    if (any(lessThan(cell, vec2(0.0))) || any(greaterThanEqual(cell, grid.xy)) || any(greaterThan(t, vec2(1.0)))) {
        frag_color = vec4(to_srgb(vec3(0.020, 0.022, 0.030)), 1.0); // the target is not sRGB
        return;
    }
    float col   = cell.x;
    float row   = grid.y - 1.0 - cell.y; // rows count from the bottom
    float h     = 2.5 + 2.2 * sin(col * 0.55) + 1.6 * cos(row * 0.8 + col * 0.2);
    float layer = min(floor(clamp(h, 0.0, 5.0)), grid.z - 1.0);
    frag_color  = vec4(to_srgb(texture(sampler2DArray(tiles, smp), vec3(t, layer)).rgb), 1.0);
}
@end

@program floor floor_vs floor_fs
