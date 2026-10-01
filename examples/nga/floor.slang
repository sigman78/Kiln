// examples/nga/floor.slang — kiln-nga-array: a floor of tiles, each a layer of one texture array; the
// same layout and "terrain" as kiln-gl-array and kiln-vk-array. FloorRoot matches main_array.cpp.
#include <NoGraphicsAPI/shader.slang>

struct FloorRoot
{
    float4 grid; // xy: columns, rows; z: the layers the bound texture has (the placeholder has one)
    uint tiles;  // descriptor index of the array
    uint pad0;
    uint pad1;
    uint pad2;
};

GPU_ROOT(FloorRoot, root);

struct FloorVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0; // 0..1, top left first (Vulkan NDC +y is down)
};

[shader("vertex")]
FloorVaryings floorVertex(uint vertex_id : SV_VertexID)
{
    FloorVaryings o;
    o.uv       = float2((vertex_id << 1) & 2, vertex_id & 2); // one triangle over the screen
    o.position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 floorFragment(FloorVaryings i) : SV_Target0
{
    float2 grid = root.grid.xy;
    float2 p    = (i.uv - 0.025) / 0.95 * grid; // the floor covers 95% of the screen
    float2 cell = floor(p);
    float2 t    = frac(p) / 0.96;               // a thin gap between tiles
    if (any(cell < 0.0) || any(cell >= grid) || any(t > 1.0))
        return float4(0.020, 0.022, 0.030, 1.0); // the target is sRGB: output stays linear
    float col   = cell.x;
    float row   = grid.y - 1.0 - cell.y; // rows count from the bottom
    float h     = 2.5 + 2.2 * sin(col * 0.55) + 1.6 * cos(row * 0.8 + col * 0.2);
    float layer = min(floor(clamp(h, 0.0, 5.0)), root.grid.z - 1.0);
    float3 c    = gpu_texture<Texture2DArray<float4>>(root.tiles).Sample(gpu_sampler(0), float3(t, layer)).rgb;
    return float4(c, 1.0);
}
