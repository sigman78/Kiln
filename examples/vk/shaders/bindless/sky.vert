#version 460
// examples/vk/shaders/bindless/sky.vert — one triangle that covers the screen; no vertex buffers.

layout(location = 0) out vec2 vNdc;

void main() {
    vec2 uv     = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); // (0,0) (2,0) (0,2)
    vNdc        = uv * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 0.0, 1.0);
}
