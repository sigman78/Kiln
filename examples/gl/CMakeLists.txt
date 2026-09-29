# examples/gl — kiln-gl: kiln over OpenGL 4.6 core, textures bound per draw
# (docs/design/integration-examples.md). No GL loader library: gl_api.cpp loads through GLFW.

add_executable(kiln-gl main.cpp gl_api.h gl_api.cpp gl_adapter.h gl_adapter.cpp)
kiln_apply_defaults(kiln-gl)
target_link_libraries(kiln-gl PRIVATE kiln_runtime kiln_cli kiln_example_common)
if(TARGET kiln_cook)
    target_link_libraries(kiln-gl PRIVATE kiln_cook)
    target_compile_definitions(kiln-gl PRIVATE KILN_GL_HAS_COOK=1)
else()
    target_compile_definitions(kiln-gl PRIVATE KILN_GL_HAS_COOK=0)
endif()
