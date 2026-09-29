# The demorig scene, view and overlay (example/common/demorig/, shared with
# the WASM build) as an ESP-IDF component.
set(DEMORIG_COMMON ${CMAKE_CURRENT_LIST_DIR}/../../../common/demorig)

# ShapoGFX is a component named after its directory: "shapo-gfx", unless the
# repository was cloned under another name
get_filename_component(SHAPOGFX_COMPONENT
    "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(SHAPOGFX_COMPONENT "${SHAPOGFX_COMPONENT}" NAME)

idf_component_register(
    SRCS
        "${DEMORIG_COMMON}/demorig.cpp"
        "${DEMORIG_COMMON}/scene.cpp"
    INCLUDE_DIRS "${DEMORIG_COMMON}"
    REQUIRES ${SHAPOGFX_COMPONENT}
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

# One texture per image rather than the atlas: the parts are read from flash
# through the cache, and without the atlas rows' padding a frame touches a
# third fewer cache lines (rgb_chan: 216 KB instead of 321 KB per frame).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DEMORIG_MODEL_HEADER="model/rgb_chan_sep.hpp")
