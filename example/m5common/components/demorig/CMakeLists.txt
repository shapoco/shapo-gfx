# The demorig scene, view and overlay (example/common/demorig/, shared with
# the WASM build) as an ESP-IDF component.
set(DEMORIG_COMMON ${CMAKE_CURRENT_LIST_DIR}/../../../common/demorig)

idf_component_register(
    SRCS
        "${DEMORIG_COMMON}/demorig.cpp"
        "${DEMORIG_COMMON}/scene.cpp"
    INCLUDE_DIRS "${DEMORIG_COMMON}"
    REQUIRES shapogfx
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
