// examples/gl/gl_background.h — the background pass shared by the two model examples.
#pragma once

#include "example_math.h"
#include "gl_api.h"

namespace kiln::glx {

struct Background {
    GLuint program = 0, vao = 0, sampler = 0;
    bool bindless = false;

    void create(bool useBindless);
    /// Before meshes: binding is a cube texture name (0 = absent), or a kiln slot (kInvalid = absent).
    /// Leaves the cube bound for mesh lighting; mesh drawing sets its own depth state.
    void draw(ex::ViewRays const& rays, f32 exposure, u32 binding) const;
    void release();
};

} // namespace kiln::glx
