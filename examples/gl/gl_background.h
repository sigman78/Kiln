// examples/gl/gl_background.h — the background pass shared by the two model examples.
#pragma once

#include "example_math.h"
#include "gl_api.h"

namespace kiln::glx {

/// Without bindless, the background samples the cube on this unit. The host binds it with a sampler.
inline constexpr GLuint kEnvironmentUnit = 5;

struct Background {
    GLuint program = 0, vao = 0;
    bool bindless = false;

    void create(bool useBindless);
    /// Draws the cube behind the meshes with no depth test or write, and leaves the depth test off.
    /// The caller checks that the cube is present. Without bindless it is bound to kEnvironmentUnit
    /// and `slot` stays kInvalid; with bindless `slot` is its kiln slot.
    void draw(ex::ViewRays const& rays, f32 exposure, u32 slot = kInvalid) const;
    void release();
};

} // namespace kiln::glx
