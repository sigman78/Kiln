# Shipping consumer: proves that an installed read-only kiln (KILN_BUILD_COOK=OFF)
# is consumable through find_package(kiln) with kiln::runtime only, and that the
# cook component is correctly reported as absent. Built standalone by CI against
# an install prefix (see .github/workflows/ci.yml, job "shipping").
cmake_minimum_required(VERSION 3.25)
project(kiln_shipping_consumer CXX)

find_package(kiln CONFIG REQUIRED)
if(NOT TARGET kiln::runtime OR NOT TARGET kiln::core)
    message(FATAL_ERROR "kiln::runtime / kiln::core missing from the installed package")
endif()
if(TARGET kiln::cook)
    message(FATAL_ERROR "kiln::cook must not be present in a shipping install")
endif()
find_package(kiln CONFIG COMPONENTS cook QUIET)
if(kiln_cook_FOUND)
    message(FATAL_ERROR "component 'cook' must be reported absent in a shipping install")
endif()

add_executable(shipping_consumer main.cpp)
target_link_libraries(shipping_consumer PRIVATE kiln::runtime)
set_target_properties(shipping_consumer PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
