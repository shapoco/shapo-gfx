# demo3d on the Waveshare RP2350-Touch-LCD-2 (320x240 landscape, two cores,
# touch). See example/rp2350-touch-lcd-2/README.md.
#
#   ../build.sh            # cmake -B build && cmake --build build
#   ../flash.sh [build/demo3d.uf2]

cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD waveshare_rp2350_touch_lcd_2)
include(${CMAKE_CURRENT_LIST_DIR}/../../rp2common/pico_project.cmake)

project(demo3d C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# -DDEMO3D_HOT=OFF leaves the rasterizer in flash (it runs from RAM by
# default: SHAPOGFX3D_HOT_ATTR, see docsrc/intro/getting_started.rst)
option(DEMO3D_HOT "Run the span rasterizer of gfx3d from RAM" ON)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../rp2common rp2common)
if(DEMO3D_HOT)
    target_compile_definitions(shapogfx PRIVATE
        "SHAPOGFX3D_HOT_ATTR=__attribute__((section(\".time_critical.gfx3d\")))"
        SHAPOGFX3D_HOT_INSTANTIATE=1)
endif()

# The scene: example/common/demo3d/ (shared with the WASM build)
set(DEMO3D_COMMON ${CMAKE_CURRENT_LIST_DIR}/../../common/demo3d)
add_executable(demo3d main.cpp ${DEMO3D_COMMON}/scene.cpp
    ${DEMO3D_COMMON}/demo3d.cpp)
target_include_directories(demo3d PRIVATE ${DEMO3D_COMMON})
target_link_libraries(demo3d PRIVATE rp2common)
rp2common_setup(demo3d)
