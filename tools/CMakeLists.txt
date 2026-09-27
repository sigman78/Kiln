# Command-line tools. kiln-info needs only kiln_runtime (read-only); kiln-cook (M2)
# will need kiln_cook.

add_executable(kiln-info kiln-info/main.cpp)
kiln_apply_defaults(kiln-info)
target_link_libraries(kiln-info PRIVATE kiln_runtime)
set_target_properties(kiln-info PROPERTIES OUTPUT_NAME kiln-info)

if(KILN_INSTALL)
    install(TARGETS kiln-info RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
