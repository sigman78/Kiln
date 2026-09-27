# examples/viewer/fetch_assets.cmake — downloads the demo glTF models (CC0, Khronos
# glTF-Sample-Assets at a pinned commit) into OUT_DIR, verifying each SHA-256. Run through
# the `viewer-assets` target or directly:
#   cmake -DOUT_DIR=examples/assets/khronos -P examples/viewer/fetch_assets.cmake
# Nothing here is committed; see examples/assets/README.md.

if(NOT OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

set(KILN_ASSETS_COMMIT 7d4ba189827916452eeadc82d4b712dbc6280a6f)
set(KILN_ASSETS_BASE "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/${KILN_ASSETS_COMMIT}/Models")

# name=sha256 (all CC0-1.0)
set(KILN_ASSETS
    Lantern=a79458c4b02d695187a952f23a63b8bf278e7bc3d316a3c2a314f2d6974181f1
    WaterBottle=b337e526fd6a162013c2984aeec163f5fbb4f717252724dfc3f3458bd51df94b
    Avocado=ccc9c3ce56423720b09399c2351537207cd5a65f859f9e6e2f30922762f3abd4
    SheenChair=f0af2a2b102d28d540236306ae19f8fb36842df76bd38cf76f063f9bd2853399
    BoomBox=f8b918445ebdd006768232205a62f5182d2208ca57f84c6ccc084943c0bc8f15)

file(MAKE_DIRECTORY ${OUT_DIR})
foreach(entry IN LISTS KILN_ASSETS)
    string(REGEX REPLACE "=.*" "" name "${entry}")
    string(REGEX REPLACE ".*=" "" sha "${entry}")
    set(dst ${OUT_DIR}/${name}.glb)
    if(EXISTS ${dst})
        file(SHA256 ${dst} have)
        if(have STREQUAL sha)
            message(STATUS "${name}.glb: present")
            continue()
        endif()
    endif()
    message(STATUS "${name}.glb: downloading")
    file(DOWNLOAD "${KILN_ASSETS_BASE}/${name}/glTF-Binary/${name}.glb" ${dst}
         EXPECTED_HASH SHA256=${sha} SHOW_PROGRESS STATUS st)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
        list(GET st 1 msg)
        message(FATAL_ERROR "${name}.glb: ${msg}")
    endif()
endforeach()
message(STATUS "demo assets in ${OUT_DIR}")
