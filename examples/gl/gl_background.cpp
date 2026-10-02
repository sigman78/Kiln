// examples/gl/gl_background.cpp — shaders and GL state for the cube background.
#include "gl_background.h"
#include "gl_util.h"

namespace kiln::glx {
namespace {
char const* const kSkyVs = R"(#version 460 core
out vec2 vNdc;
void main() {
    vNdc        = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2) * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 1.0, 1.0);
}
)";

constexpr char const* kSkyFs = R"(
in vec2 vNdc;
layout(binding = 5) uniform samplerCube uSky; // kEnvironmentUnit
layout(location = 0) uniform vec3 uForward;
layout(location = 1) uniform vec3 uRight;
layout(location = 2) uniform vec3 uUp;
layout(location = 4) uniform float uExposure;
out vec4 outColor;
void main() {
    vec3 dir = normalize(uForward + vNdc.x * uRight + vNdc.y * uUp);
    outColor = display(texture(uSky, cube_dir(dir)).rgb, uExposure);
}
)";

constexpr char const* kBindlessSkyFs = R"(
in vec2 vNdc;
layout(std430, binding = 0) readonly buffer Handles { uvec2 uHandles[]; };
layout(location = 0) uniform vec3 uForward;
layout(location = 1) uniform vec3 uRight;
layout(location = 2) uniform vec3 uUp;
layout(location = 4) uniform float uExposure;
layout(location = 5) uniform uint uSkySlot;
out vec4 outColor;
void main() {
    vec3 dir = normalize(uForward + vNdc.x * uRight + vNdc.y * uUp);
    outColor = display(texture(samplerCube(uHandles[uSkySlot]), cube_dir(dir)).rgb, uExposure);
}
)";

} // namespace

void Background::create(bool useBindless) {
    bindless = useBindless;
    program  = build_program(kSkyVs, bindless ? "#extension GL_ARB_bindless_texture : require\n" : "",
                            bindless ? kBindlessSkyFs : kSkyFs);
    glCreateVertexArrays(1, &vao);
}

void Background::draw(ex::ViewRays const& rays, f32 exposure, u32 slot) const {
    glUseProgram(program);
    glUniform3f(0, rays.forward.x, rays.forward.y, rays.forward.z);
    glUniform3f(1, rays.right.x, rays.right.y, rays.right.z);
    glUniform3f(2, rays.up.x, rays.up.y, rays.up.z);
    glUniform1f(4, exposure);
    if (bindless) glUniform1ui(5, slot);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthMask(GL_TRUE);
}

void Background::release() {
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    program = vao = 0;
}

} // namespace kiln::glx
