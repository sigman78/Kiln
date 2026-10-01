# examples/viewer — kiln-viewer and kiln-vk-smoke (docs/design/viewer.md), on kiln_example_vk
# (examples/vk: the device, the example adapter and the frame plumbing).

add_executable(kiln-viewer main.cpp viewer_math.h)
target_link_libraries(kiln-viewer PRIVATE kiln_no_crash_dialogs)
kiln_apply_defaults(kiln-viewer)
target_include_directories(kiln-viewer PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(kiln-viewer PRIVATE GLFW_INCLUDE_NONE)
target_link_libraries(kiln-viewer PRIVATE kiln_example_vk kiln_cli glfw)
if(TARGET kiln_cook)
    target_link_libraries(kiln-viewer PRIVATE kiln_cook)
    target_compile_definitions(kiln-viewer PRIVATE KILN_VIEWER_HAS_COOK=1)
else()
    target_compile_definitions(kiln-viewer PRIVATE KILN_VIEWER_HAS_COOK=0)
endif()

# Loads the golden store through the real adapter with no window; runs only by hand
# (needs a Vulkan driver), so it is not a CTest.
add_executable(kiln-vk-smoke adapter_smoke.cpp)
target_link_libraries(kiln-vk-smoke PRIVATE kiln_no_crash_dialogs)
kiln_apply_defaults(kiln-vk-smoke)
target_link_libraries(kiln-vk-smoke PRIVATE kiln_example_vk kiln_cli)

# viewer-assets (the demo models) is defined in examples/common.

# One command to see it: fetch the models, cook them on the fly into the build tree's
# demo-store, open the viewer. `cmake --build --preset <p> --target viewer-demo`. The models
# are glb files, so a KILN_MESH=OFF build has no demo.
if(KILN_MESH)
    set(KILN_VIEWER_DEMO_MODELS Lantern.glb WaterBottle.glb Avocado.glb SheenChair.glb BoomBox.glb
                                DiffuseTransmissionTeacup.glb)
    add_custom_target(viewer-demo
        COMMAND $<TARGET_FILE:kiln-viewer> --source ${CMAKE_SOURCE_DIR}/examples/assets/khronos
                --store ${CMAKE_BINARY_DIR}/demo-store ${KILN_VIEWER_DEMO_MODELS}
        DEPENDS kiln-viewer viewer-assets
        USES_TERMINAL
        COMMENT "kiln-viewer on the demo models (Esc quits)")
endif()
