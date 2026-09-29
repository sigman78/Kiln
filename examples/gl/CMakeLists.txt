# examples/gl — kiln over OpenGL 4.6 core (docs/design/integration-examples.md):
#   kiln-gl           textures bound per draw           (KILN_EXAMPLE_GL)
#   kiln-gl-bindless  ARB_bindless_texture handle table (KILN_EXAMPLE_GL_BINDLESS)
# No GL loader library: gl_api.cpp loads through GLFW.

add_library(kiln_example_gl STATIC gl_api.h gl_api.cpp gl_adapter.h gl_adapter.cpp gl_util.h gl_util.cpp)
kiln_apply_defaults(kiln_example_gl)
target_include_directories(kiln_example_gl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_example_gl PUBLIC kiln_runtime kiln_example_common PRIVATE kiln_cli)

function(kiln_gl_example name source)
    add_executable(${name} ${source})
    kiln_apply_defaults(${name})
    target_link_libraries(${name} PRIVATE kiln_example_gl)
    if(TARGET kiln_cook)
        target_link_libraries(${name} PRIVATE kiln_cook)
        target_compile_definitions(${name} PRIVATE KILN_GL_HAS_COOK=1)
    else()
        target_compile_definitions(${name} PRIVATE KILN_GL_HAS_COOK=0)
    endif()
endfunction()

if(KILN_EXAMPLE_GL)
    kiln_gl_example(kiln-gl main.cpp)
endif()
if(KILN_EXAMPLE_GL_BINDLESS)
    kiln_gl_example(kiln-gl-bindless main_bindless.cpp)
endif()
