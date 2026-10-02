// examples/vk/shaders/array/floor.glsl — the floor both fragment shaders draw: a grid of tiles, each
// sampling one layer of the array. Included after `sample_tile(uv, layer)` is defined. The target is
// sRGB: the output stays linear.

// grid.xy: columns and rows; grid.z: available layers; slot: the array's bindless slot.
layout(push_constant) uniform Floor {
    vec4 grid;
    uint slot;
} floorPush;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 grid = floorPush.grid.xy;
    vec2 p    = (vUv - 0.025) / 0.95 * grid; // the floor covers 95% of the screen
    vec2 cell = floor(p);
    vec2 t    = fract(p) / 0.96;              // a thin gap between tiles
    if (any(lessThan(cell, vec2(0))) || any(greaterThanEqual(cell, grid)) || any(greaterThan(t, vec2(1)))) {
        outColor = vec4(0.020, 0.022, 0.030, 1.0);
        return;
    }
    // The same "terrain" as kiln-gl-array: rows count from the bottom.
    float col   = cell.x;
    float row   = grid.y - 1.0 - cell.y;
    float h     = 2.5 + 2.2 * sin(col * 0.55) + 1.6 * cos(row * 0.8 + col * 0.2);
    float layer = min(floor(clamp(h, 0.0, 5.0)), floorPush.grid.z - 1.0);
    outColor    = vec4(sample_tile(t, layer).rgb, 1.0);
}
