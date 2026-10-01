# Command-line tools. kiln-info needs only kiln_runtime (read-only); kiln-cook (M2)
# will need kiln_cook.

add_executable(kiln-info kiln-info/main.cpp)
kiln_apply_defaults(kiln-info)
target_link_libraries(kiln-info PRIVATE kiln_runtime kiln_cli kiln_no_crash_dialogs)
set_target_properties(kiln-info PROPERTIES OUTPUT_NAME kiln-info)

if(KILN_INSTALL)
    install(TARGETS kiln-info RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# kiln-cook: dev/CI cooker CLI. Needs the cook side; absent in shipping builds.
if(TARGET kiln_cook)
    add_executable(kiln-cook kiln-cook/main.cpp)
    kiln_apply_defaults(kiln-cook)
    target_link_libraries(kiln-cook PRIVATE kiln_cook kiln_cli kiln_no_crash_dialogs)
    set_target_properties(kiln-cook PROPERTIES OUTPUT_NAME kiln-cook)
    if(KILN_INSTALL)
        install(TARGETS kiln-cook RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT cook)
    endif()
endif()
