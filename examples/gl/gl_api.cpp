// examples/gl/gl_api.cpp — loads the GL function table through glfwGetProcAddress.
#include "gl_api.h"

#include <kiln/log.h>

#include <GLFW/glfw3.h>

namespace kiln::glx {

#define KILN_GL_DEFINE(ret, name, params) PFN_##name name = nullptr;
KILN_GL_FUNCTIONS(KILN_GL_DEFINE)
#undef KILN_GL_DEFINE

bool load_gl() noexcept {
    bool ok = true;
#define KILN_GL_LOAD(ret, name, params)                                                                      \
    name = reinterpret_cast<PFN_##name>(glfwGetProcAddress(#name));                                          \
    if (!name && ok) {                                                                                       \
        KILN_ERROR("gl", "missing %s: the example needs OpenGL 4.6 core", #name);                            \
        ok = false;                                                                                          \
    }
    KILN_GL_FUNCTIONS(KILN_GL_LOAD)
#undef KILN_GL_LOAD
    return ok;
}

} // namespace kiln::glx
