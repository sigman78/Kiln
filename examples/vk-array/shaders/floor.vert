#version 460
// examples/vk-array/shaders/floor.vert — one triangle that covers the screen; no vertex buffers.

layout(location = 0) out vec2 vUv; // 0..1, top left first

void main() {
    vec2 uv     = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); // (0,0) (2,0) (0,2)
    vUv         = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
