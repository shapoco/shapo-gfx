# ShapoGFX (this repository) as an ESP-IDF component: gfx2d only.
set(SHAPOGFX_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../..)

idf_component_register(
    SRCS
        "${SHAPOGFX_ROOT}/src/gfx2d/graphics2d.cpp"
        "${SHAPOGFX_ROOT}/src/gfx2d/images.cpp"
        "${SHAPOGFX_ROOT}/src/gfx2d/rig.cpp"
        "${SHAPOGFX_ROOT}/src/gfx2d/shapes.cpp"
    INCLUDE_DIRS "${SHAPOGFX_ROOT}/include"
)

# demorig draws into RGB565_SWAPPED (the byte order the SPI panels want on
# the wire, and the PPA swaps it for the Tab5) from an ARGB4444 atlas, so
# GRAY1 and RGB444 come out (the output is bit-identical). A build property,
# not a target one: the format macros are read by the headers as well, so
# every translation unit has to agree.
idf_build_set_property(COMPILE_DEFINITIONS "SHAPOGFX_FORMAT_GRAY1=0" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "SHAPOGFX_FORMAT_RGB444=0" APPEND)
